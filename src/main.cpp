#include "app/CommandLine.hpp"
#include "media/RtspDecoder.hpp"
#include "media/VulkanHwDevice.hpp"
#include "platform/SDLPlatform.hpp"
#include "video/RawFrameMailbox.hpp"
#include "vulkan/VulkanContext.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

int main(int argc, char** argv) {
    try {
        const auto options = reg::app::parseCommandLine(argc, argv);

        reg::platform::SDLPlatform platform;
        SDL_Window* window = platform.createVulkanWindow(
            "Reg - Phase A Vulkan Decode Probe",
            1280,
            720);

        reg::vulkan::VulkanContext vulkan(window, options.validation);
        reg::media::VulkanHwDevice hwDevice(vulkan);

        reg::media::RtspDecoder decoder(
            hwDevice.ref(),
            reg::media::RtspDecoderConfig{
                .url = options.rtspUrl,
                .maxDelayUs = options.maxDelayUs,
                .reorderQueueSize = options.reorderQueueSize,
                .extraHwFrames = options.extraHwFrames,
            });

        reg::video::RawFrameMailbox rawMailbox;
        std::atomic_uint64_t decodedFrames{0};
        std::atomic_bool decoderFinished{false};
        std::mutex errorMutex;
        std::exception_ptr decoderError;

        std::jthread decodeThread([&] {
            try {
                decoder.run([&](reg::video::VideoFramePtr frame) {
                    rawMailbox.publish(frame);
                    const auto count = decodedFrames.fetch_add(1, std::memory_order_relaxed) + 1;

                    if (count == 1 || count % 120 == 0) {
                        std::cout << "[decoder] frame=" << count
                                  << " size=" << frame->width() << 'x' << frame->height();

                        if (const auto identity = frame->identity()) {
                            std::cout << " epoch=" << identity->key.streamEpoch
                                      << " source_frame=" << identity->key.frameId;
                        } else {
                            std::cout << " sei_frame_id=absent";
                        }
                        std::cout << '\n';
                    }
                });
            } catch (...) {
                std::scoped_lock lock(errorMutex);
                decoderError = std::current_exception();
            }
            decoderFinished.store(true, std::memory_order_release);
        });

        while (!decoderFinished.load(std::memory_order_acquire)) {
            if (platform.pollQuitRequested()) {
                decoder.requestStop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        decoder.requestStop();
        if (decodeThread.joinable()) {
            decodeThread.join();
        }

        {
            std::scoped_lock lock(errorMutex);
            if (decoderError) {
                std::rethrow_exception(decoderError);
            }
        }

        std::cout << "[probe] decoded Vulkan frames: "
                  << decodedFrames.load(std::memory_order_relaxed) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        reg::app::printUsage(argc > 0 ? argv[0] : "reg_probe");
        return 1;
    }
}
