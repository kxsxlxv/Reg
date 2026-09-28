#include "media/RtspDecoder.hpp"
#include "media/VulkanHwDevice.hpp"
#include "platform/SDLPlatform.hpp"
#include "render/ImGuiOverlayRenderer.hpp"
#include "render/TargetOverlayBuilder.hpp"
#include "replay/ReplayClock.hpp"
#include "replay/ReplayMetadataIndex.hpp"
#include "video/VideoTransform.hpp"
#include "vulkan/RenderWindow.hpp"
#include "vulkan/VideoRenderer.hpp"
#include "vulkan/VulkanContext.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

struct ReplayOptions {
    std::filesystem::path video;
    std::filesystem::path metadataDirectory;
    double speed{1.0};
    bool validation{true};
};

ReplayOptions parseOptions(
    int argc,
    char** argv) {
    ReplayOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);

        const auto requireValue =
            [&](const char* name) -> std::string_view {
                if (i + 1 >= argc) {
                    throw std::runtime_error(
                        std::string("Missing value for ") +
                        name);
                }
                ++i;
                return argv[i];
            };

        if (arg == "--video") {
            options.video =
                std::filesystem::path(
                    requireValue("--video"));
        } else if (arg == "--metadata-dir") {
            options.metadataDirectory =
                std::filesystem::path(
                    requireValue("--metadata-dir"));
        } else if (arg == "--speed") {
            const std::string text(
                requireValue("--speed"));
            std::size_t consumed = 0;
            options.speed =
                std::stod(
                    text,
                    &consumed);
            if (consumed != text.size() ||
                !std::isfinite(options.speed) ||
                !(options.speed > 0.0)) {
                throw std::runtime_error(
                    "--speed must be positive");
            }
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (
            arg == "--help" ||
            arg == "-h") {
            std::cout
                << "Usage: reg_replay --video FILE.mkv "
                   "--metadata-dir DIR [--speed N] [--no-validation]\n";
            std::exit(0);
        } else {
            throw std::runtime_error(
                "Unknown replay argument: " +
                std::string(arg));
        }
    }

    if (options.video.empty()) {
        throw std::runtime_error(
            "--video is required");
    }
    if (options.metadataDirectory.empty()) {
        throw std::runtime_error(
            "--metadata-dir is required");
    }
    if (!std::filesystem::exists(
            options.video)) {
        throw std::runtime_error(
            "Replay video does not exist: " +
            options.video.string());
    }

    return options;
}

class ReplayFrameQueue final {
public:
    void push(
        reg::video::VideoFramePtr frame) {
        std::unique_lock lock(mutex_);

        condition_.wait(
            lock,
            [&] {
                return stopping_ ||
                       !frame_;
            });

        if (stopping_) {
            return;
        }

        frame_ = std::move(frame);
        condition_.notify_all();
    }

    reg::video::VideoFramePtr tryTake() {
        std::scoped_lock lock(mutex_);

        if (!frame_) {
            return {};
        }

        auto frame =
            std::move(frame_);
        frame_.reset();
        condition_.notify_all();
        return frame;
    }

    bool empty() const {
        std::scoped_lock lock(mutex_);
        return !frame_;
    }

    void stop() noexcept {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            frame_.reset();
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    reg::video::VideoFramePtr frame_;
    bool stopping_{false};
};

std::optional<std::chrono::nanoseconds>
frameMediaTime(
    const reg::video::VideoFrame& frame,
    AVRational timeBase) {
    const AVFrame* avFrame =
        frame.avFrame();

    if (avFrame == nullptr ||
        avFrame->best_effort_timestamp ==
            AV_NOPTS_VALUE) {
        return std::nullopt;
    }

    const std::int64_t ns =
        av_rescale_q(
            avFrame->best_effort_timestamp,
            timeBase,
            AVRational{
                1,
                1000000000,
            });

    return std::chrono::nanoseconds{ns};
}

void sleepUntilReplayTarget(
    std::chrono::steady_clock::time_point target,
    const std::atomic_bool& stopRequested) {
    while (!stopRequested.load(
        std::memory_order_acquire)) {
        const auto now =
            std::chrono::steady_clock::now();

        if (now >= target) {
            return;
        }

        const auto remaining =
            target - now;

        const auto step =
            std::min(
                remaining,
                std::chrono::steady_clock::duration{
                    std::chrono::milliseconds{5}});

        std::this_thread::sleep_for(step);
    }
}

} // namespace

int main(
    int argc,
    char** argv) {
    try {
        const ReplayOptions options =
            parseOptions(argc, argv);

        reg::replay::ReplayMetadataIndex
            metadataIndex;
        metadataIndex.loadDirectory(
            options.metadataDirectory);

        std::cout
            << "[replay] metadata records indexed: "
            << metadataIndex.size()
            << '\n';

        reg::platform::SDLPlatform platform;

        SDL_Window* rawSdlWindow =
            platform.createVulkanWindow(
                "Reg Replay - Raw",
                1280,
                720);

        static_cast<void>(
            platform.placeWindowOnDisplay(
                rawSdlWindow,
                0));

        reg::vulkan::VulkanContext vulkan(
            rawSdlWindow,
            options.validation);

        reg::vulkan::RenderWindow rawWindow(
            vulkan,
            rawSdlWindow,
            reg::vulkan::PresentPolicy::
                LowLatencyTearingAllowed);

        SDL_Window* overlaySdlWindow =
            platform.createVulkanWindow(
                "Reg Replay - Exact Overlay",
                1280,
                720);

        static_cast<void>(
            platform.placeWindowOnDisplay(
                overlaySdlWindow,
                1));

        reg::vulkan::RenderWindow overlayWindow(
            vulkan,
            overlaySdlWindow,
            reg::vulkan::PresentPolicy::Stable);

        reg::media::VulkanHwDevice hwDevice(
            vulkan);

        reg::media::RtspDecoder decoder(
            hwDevice.ref(),
            reg::media::RtspDecoderConfig{
                .url = options.video.string(),
                .inputKind =
                    reg::media::VideoInputKind::File,
                .extraHwFrames = 32,
            });

        reg::vulkan::VideoRenderer rawRenderer(
            vulkan);
        reg::vulkan::VideoRenderer overlayRenderer(
            vulkan);

        reg::render::ImGuiOverlayRenderer
            overlaySceneRenderer(
                vulkan,
                overlayWindow.swapchain());

        reg::render::TargetOverlayBuilder
            overlayBuilder;

        reg::metadata::TrackHistory trackHistory(
            std::chrono::seconds{5},
            1024);

        ReplayFrameQueue frameQueue;
        reg::replay::ReplayClock replayClock(
            options.speed);

        std::atomic_bool stopRequested{false};
        std::atomic_bool decodeFinished{false};

        std::mutex decodeErrorMutex;
        std::exception_ptr decodeError;

        AVRational inputTimeBase{0, 1};

        std::jthread decodeThread([&] {
            try {
                decoder.run(
                    reg::media::RtspDecoderCallbacks{
                        .onStreamOpened =
                            [&](reg::media::VideoStreamDescriptorPtr descriptor) {
                                inputTimeBase =
                                    descriptor->timeBase();
                            },
                        .onFrame =
                            [&](reg::video::VideoFramePtr frame) {
                                if (const auto mediaTime =
                                        frameMediaTime(
                                            *frame,
                                            inputTimeBase)) {
                                    const auto target =
                                        replayClock.targetTime(
                                            *mediaTime);

                                    sleepUntilReplayTarget(
                                        target,
                                        stopRequested);
                                }

                                if (!stopRequested.load(
                                        std::memory_order_acquire)) {
                                    frameQueue.push(
                                        std::move(frame));
                                }
                            },
                    });
            } catch (...) {
                std::scoped_lock lock(
                    decodeErrorMutex);
                decodeError =
                    std::current_exception();
            }

            decodeFinished.store(
                true,
                std::memory_order_release);
        });

        const auto stopAndJoin = [&] {
            stopRequested.store(
                true,
                std::memory_order_release);
            decoder.requestStop();
            frameQueue.stop();

            if (decodeThread.joinable()) {
                decodeThread.join();
            }
        };

        reg::video::VideoFramePtr currentFrame;
        reg::metadata::FrameMetadataPtr currentMetadata;
        bool rawDone = false;
        bool overlayDone = false;

        std::uint64_t presentedFrames = 0;
        std::uint64_t overlayFrames = 0;
        std::uint64_t missingIdentity = 0;
        std::uint64_t missingMetadata = 0;

        try {
            while (!decodeFinished.load(
                       std::memory_order_acquire) ||
                   currentFrame ||
                   !frameQueue.empty()) {
                if (platform.pollQuitRequested()) {
                    break;
                }

                if (!currentFrame) {
                    currentFrame =
                        frameQueue.tryTake();

                    if (currentFrame) {
                        rawDone = false;
                        overlayDone = false;
                        currentMetadata.reset();

                        if (const auto identity =
                                currentFrame->identity()) {
                            currentMetadata =
                                metadataIndex.find(
                                    identity->key);

                            if (!currentMetadata) {
                                ++missingMetadata;
                                overlayDone = true;
                            } else {
                                trackHistory.update(
                                    *currentMetadata,
                                    std::chrono::steady_clock::now());
                            }
                        } else {
                            ++missingIdentity;
                            overlayDone = true;
                        }
                    }
                }

                bool didWork = false;

                if (currentFrame &&
                    !rawDone) {
                    if (rawRenderer.render(
                            currentFrame,
                            rawWindow.swapchain())) {
                        rawDone = true;
                        ++presentedFrames;
                        didWork = true;
                    }
                }

                if (currentFrame &&
                    currentMetadata &&
                    !overlayDone) {
                    const auto extent =
                        overlayWindow.swapchain()
                            .extent();

                    reg::video::VideoTransform
                        transform(
                            static_cast<std::uint32_t>(
                                currentFrame->width()),
                            static_cast<std::uint32_t>(
                                currentFrame->height()),
                            extent.width,
                            extent.height);

                    const auto scene =
                        overlayBuilder.build(
                            *currentMetadata,
                            trackHistory,
                            transform);

                    overlaySceneRenderer.prepare(
                        scene,
                        overlayWindow.swapchain());

                    if (overlayRenderer.render(
                            currentFrame,
                            overlayWindow.swapchain(),
                            &overlaySceneRenderer)) {
                        overlayDone = true;
                        ++overlayFrames;
                        didWork = true;
                    }
                }

                if (currentFrame &&
                    rawDone &&
                    overlayDone) {
                    currentFrame.reset();
                    currentMetadata.reset();
                    didWork = true;
                }

                if (!didWork) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{1});
                }
            }
        } catch (...) {
            stopAndJoin();
            throw;
        }

        stopAndJoin();

        {
            std::scoped_lock lock(
                decodeErrorMutex);
            if (decodeError) {
                std::rethrow_exception(
                    decodeError);
            }
        }

        std::cout
            << "[replay] raw presented="
            << presentedFrames
            << " overlay presented="
            << overlayFrames
            << " missing_identity="
            << missingIdentity
            << " missing_metadata="
            << missingMetadata
            << '\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "fatal replay error: "
            << error.what()
            << '\n';
        return 1;
    }
}
