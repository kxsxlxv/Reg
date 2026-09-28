#include "app/CommandLine.hpp"
#include "media/RtspDecoder.hpp"
#include "media/VulkanHwDevice.hpp"
#include "metadata/MetadataReceiver.hpp"
#include "metadata/MetadataStore.hpp"
#include "metadata/TrackHistory.hpp"
#include "platform/SDLPlatform.hpp"
#include "render/ImGuiOverlayRenderer.hpp"
#include "render/TargetOverlayBuilder.hpp"
#include "recorder/BlackboxRecorder.hpp"
#include "video/FrameSynchronizer.hpp"
#include "video/OverlayFrameBuffer.hpp"
#include "video/RawFrameMailbox.hpp"
#include "video/VideoTransform.hpp"
#include "vulkan/RenderWindow.hpp"
#include "vulkan/VideoRenderer.hpp"
#include "vulkan/VulkanContext.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

int main(int argc, char** argv) {
    try {
        const auto options = reg::app::parseCommandLine(argc, argv);

        reg::platform::SDLPlatform platform;

        SDL_Window* rawSdlWindow = platform.createVulkanWindow(
            "Reg - Raw",
            1280,
            720);

        reg::vulkan::VulkanContext vulkan(
            rawSdlWindow,
            options.validation);

        reg::vulkan::RenderWindow rawWindow(
            vulkan,
            rawSdlWindow,
            reg::vulkan::PresentPolicy::LowLatencyTearingAllowed);

        std::unique_ptr<reg::vulkan::RenderWindow> overlayWindow;

        if (options.overlayEnabled) {
            SDL_Window* overlaySdlWindow = platform.createVulkanWindow(
                "Reg - Exact CV Overlay",
                1280,
                720);

            overlayWindow = std::make_unique<reg::vulkan::RenderWindow>(
                vulkan,
                overlaySdlWindow,
                reg::vulkan::PresentPolicy::Stable);
        }

        reg::media::VulkanHwDevice hwDevice(vulkan);

        reg::media::RtspDecoder decoder(
            hwDevice.ref(),
            reg::media::RtspDecoderConfig{
                .url = options.rtspUrl,
                .maxDelayUs = options.maxDelayUs,
                .reorderQueueSize = options.reorderQueueSize,
                .extraHwFrames = options.extraHwFrames,
            });

        std::unique_ptr<reg::recorder::BlackboxRecorder>
            blackboxRecorder;

        if (options.recorderEnabled) {
            blackboxRecorder =
                std::make_unique<
                    reg::recorder::BlackboxRecorder>(
                    reg::recorder::BlackboxRecorderConfig{
                        .writer =
                            reg::recorder::SegmentWriterConfig{
                                .directory =
                                    options.recordDirectory,
                                .targetSegmentDuration =
                                    std::chrono::milliseconds{
                                        options.recordSegmentMs},
                                .retention =
                                    std::chrono::seconds{
                                        options.recordRetentionSeconds},
                            },
                        .metadataJournal =
                            reg::recorder::MetadataJournalConfig{
                                .directory =
                                    options.recordDirectory,
                                .targetSegmentDuration =
                                    std::chrono::milliseconds{
                                        options.recordSegmentMs},
                                .retention =
                                    std::chrono::seconds{
                                        options.recordRetentionSeconds},
                            },
                        .queueCapacity =
                            static_cast<std::size_t>(
                                options.recordQueueCapacity),
                    });
        }

        reg::video::RawFrameMailbox rawMailbox;
        reg::video::OverlayFrameBuffer overlayFrames(
            std::chrono::milliseconds{options.overlayDelayMs},
            64);
        reg::metadata::MetadataStore metadataStore(512);
        reg::video::FrameSynchronizer synchronizer(
            overlayFrames,
            metadataStore);

        reg::vulkan::VideoRenderer rawRenderer(vulkan);
        std::unique_ptr<reg::vulkan::VideoRenderer> overlayRenderer;
        std::unique_ptr<reg::render::ImGuiOverlayRenderer>
            overlaySceneRenderer;

        reg::metadata::TrackHistory trackHistory(
            std::chrono::seconds{5},
            1024);
        reg::render::TargetOverlayBuilder overlayBuilder;

        if (options.overlayEnabled) {
            // Construct after the decoder so renderer-owned VkImageViews are
            // destroyed before FFmpeg releases its hardware-frame pool.
            overlayRenderer =
                std::make_unique<reg::vulkan::VideoRenderer>(vulkan);

            overlaySceneRenderer =
                std::make_unique<
                    reg::render::ImGuiOverlayRenderer>(
                        vulkan,
                        overlayWindow->swapchain());
        }

        std::unique_ptr<reg::metadata::MetadataReceiver> metadataReceiver;
        if (options.overlayEnabled) {
            metadataReceiver =
                std::make_unique<reg::metadata::MetadataReceiver>(
                    metadataStore,
                    reg::metadata::MetadataReceiverConfig{
                        .bindAddress = options.metadataBindAddress,
                        .port = options.metadataPort,
                        .receiveTimeoutMs = 100,
                    },
                    [&](reg::metadata::FrameMetadata metadata) {
                        if (blackboxRecorder) {
                            static_cast<void>(
                                blackboxRecorder->submitMetadata(
                                    std::move(metadata)));
                        }
                    });
        }

        std::atomic_uint64_t decodedFrames{0};
        std::atomic_uint64_t rawPresentedFrames{0};
        std::atomic_uint64_t overlayPresentedFrames{0};
        std::atomic_uint64_t overlayMissingIdentity{0};
        std::atomic_uint64_t overlayBufferEvictions{0};
        std::atomic_uint64_t overlayMissingMetadataDrops{0};

        std::atomic_bool shutdownRequested{false};
        std::atomic_bool decoderFinished{false};
        std::atomic_bool decoderConnected{false};
        std::atomic_bool metadataFailed{false};

        std::atomic_uint64_t decoderSessionGeneration{0};
        std::atomic_uint64_t decoderReconnects{0};

        std::mutex errorMutex;
        std::string lastDecoderError;
        std::exception_ptr metadataError;

        std::jthread decodeThread([&] {
            int reconnectDelayMs =
                options.reconnectInitialMs;

            while (!shutdownRequested.load(
                std::memory_order_acquire)) {
                bool sessionOpened = false;

                try {
                    decoder.run(
                        reg::media::RtspDecoderCallbacks{
                            .onStreamOpened =
                                [&](reg::media::VideoStreamDescriptorPtr descriptor) {
                                    // The new session owns a fresh stream identity
                                    // domain. Remove stale frame/metadata state before
                                    // any frame from this session can be published.
                                    rawMailbox.clear();
                                    overlayFrames.clear();
                                    metadataStore.clear();

                                    if (blackboxRecorder) {
                                        blackboxRecorder->configure(
                                            std::move(descriptor));
                                    }

                                    sessionOpened = true;
                                    reconnectDelayMs =
                                        options.reconnectInitialMs;

                                    decoderConnected.store(
                                        true,
                                        std::memory_order_release);

                                    decoderSessionGeneration.fetch_add(
                                        1,
                                        std::memory_order_acq_rel);

                                    std::cout
                                        << "[watchdog] RTSP session opened\n";
                                },
                            .onCompressedPacket =
                                [&](reg::media::CompressedVideoPacketPtr packet) {
                                    if (blackboxRecorder) {
                                        static_cast<void>(
                                            blackboxRecorder->submit(
                                                std::move(packet)));
                                    }
                                },
                            .onFrame =
                                [&](reg::video::VideoFramePtr frame) {
                                    rawMailbox.publish(frame);

                                    if (options.overlayEnabled) {
                                        const auto pushResult =
                                            overlayFrames.push(frame);

                                        if (pushResult ==
                                            reg::video::OverlayPushResult::
                                                MissingIdentity) {
                                            overlayMissingIdentity.fetch_add(
                                                1,
                                                std::memory_order_relaxed);
                                        } else if (
                                            pushResult ==
                                            reg::video::OverlayPushResult::
                                                EvictedOldest) {
                                            overlayBufferEvictions.fetch_add(
                                                1,
                                                std::memory_order_relaxed);
                                        }
                                    }

                                    const auto count =
                                        decodedFrames.fetch_add(
                                            1,
                                            std::memory_order_relaxed) +
                                        1;

                                    if (count == 1 ||
                                        count % 120 == 0) {
                                        std::cout
                                            << "[decoder] frame="
                                            << count
                                            << " size="
                                            << frame->width()
                                            << 'x'
                                            << frame->height();

                                        if (const auto identity =
                                                frame->identity()) {
                                            std::cout
                                                << " epoch="
                                                << identity->key.streamEpoch
                                                << " source_frame="
                                                << identity->key.frameId;
                                        } else {
                                            std::cout
                                                << " sei_frame_id=absent";
                                        }

                                        std::cout << '\n';
                                    }
                                },
                        });

                    decoderConnected.store(
                        false,
                        std::memory_order_release);

                    if (shutdownRequested.load(
                            std::memory_order_acquire)) {
                        break;
                    }

                    {
                        std::scoped_lock lock(errorMutex);
                        lastDecoderError =
                            "RTSP decoder session ended unexpectedly";
                    }
                } catch (const std::exception& error) {
                    decoderConnected.store(
                        false,
                        std::memory_order_release);

                    if (shutdownRequested.load(
                            std::memory_order_acquire)) {
                        break;
                    }

                    {
                        std::scoped_lock lock(errorMutex);
                        lastDecoderError = error.what();
                    }

                    std::cerr
                        << "[watchdog] RTSP session failed: "
                        << error.what()
                        << '\n';
                } catch (...) {
                    decoderConnected.store(
                        false,
                        std::memory_order_release);

                    if (shutdownRequested.load(
                            std::memory_order_acquire)) {
                        break;
                    }

                    {
                        std::scoped_lock lock(errorMutex);
                        lastDecoderError =
                            "unknown RTSP decoder failure";
                    }

                    std::cerr
                        << "[watchdog] RTSP session failed: unknown error\n";
                }

                if (shutdownRequested.load(
                        std::memory_order_acquire)) {
                    break;
                }

                const auto reconnectNumber =
                    decoderReconnects.fetch_add(
                        1,
                        std::memory_order_relaxed) +
                    1;

                std::cerr
                    << "[watchdog] reconnect="
                    << reconnectNumber
                    << " delay_ms="
                    << reconnectDelayMs
                    << '\n';

                constexpr int sleepQuantumMs = 25;
                int sleptMs = 0;

                while (sleptMs < reconnectDelayMs &&
                       !shutdownRequested.load(
                           std::memory_order_acquire)) {
                    const int remaining =
                        reconnectDelayMs - sleptMs;
                    const int step =
                        std::min(
                            remaining,
                            sleepQuantumMs);

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{step});
                    sleptMs += step;
                }

                if (!sessionOpened) {
                    reconnectDelayMs =
                        std::min(
                            reconnectDelayMs * 2,
                            options.reconnectMaxMs);
                }
            }

            decoderConnected.store(
                false,
                std::memory_order_release);

            decoderFinished.store(
                true,
                std::memory_order_release);
        });

        std::jthread metadataThread;
        if (metadataReceiver) {
            metadataThread = std::jthread([&] {
                try {
                    metadataReceiver->run();
                } catch (...) {
                    {
                        std::scoped_lock lock(errorMutex);
                        metadataError = std::current_exception();
                    }
                    metadataFailed.store(
                        true,
                        std::memory_order_release);
                }
            });
        }

        const auto stopAndJoin = [&] {
            shutdownRequested.store(
                true,
                std::memory_order_release);

            decoder.requestStop();

            if (metadataReceiver) {
                metadataReceiver->requestStop();
            }

            if (decodeThread.joinable()) {
                decodeThread.join();
            }
            if (metadataThread.joinable()) {
                metadataThread.join();
            }

            if (blackboxRecorder) {
                blackboxRecorder->stop();
            }
        };

        reg::video::VideoFramePtr lastRawPresented;
        std::optional<reg::video::SynchronizedFrame>
            pendingOverlayFrame;
        std::uint64_t observedSessionGeneration{0};

        try {
            while (!decoderFinished.load(
                std::memory_order_acquire)) {
                if (platform.pollQuitRequested()) {
                    break;
                }

                if (metadataFailed.load(
                        std::memory_order_acquire)) {
                    break;
                }

                bool didWork = false;

                const std::uint64_t sessionGeneration =
                    decoderSessionGeneration.load(
                        std::memory_order_acquire);

                if (sessionGeneration !=
                    observedSessionGeneration) {
                    observedSessionGeneration =
                        sessionGeneration;
                    pendingOverlayFrame.reset();
                    lastRawPresented.reset();
                    trackHistory.clear();
                    didWork = true;
                }

                const auto latest = rawMailbox.latest();
                if (latest && latest != lastRawPresented) {
                    if (rawRenderer.render(
                            latest,
                            rawWindow.swapchain())) {
                        lastRawPresented = latest;
                        didWork = true;

                        const auto count =
                            rawPresentedFrames.fetch_add(
                                1,
                                std::memory_order_relaxed) +
                            1;

                        if (count == 1 || count % 120 == 0) {
                            std::cout
                                << "[raw] presented="
                                << count
                                << " source="
                                << latest->width()
                                << 'x'
                                << latest->height()
                                << " output="
                                << rawWindow.swapchain().extent().width
                                << 'x'
                                << rawWindow.swapchain().extent().height
                                << '\n';
                        }
                    }
                }

                if (options.overlayEnabled &&
                    overlayWindow &&
                    overlayRenderer) {
                    if (!pendingOverlayFrame) {
                        // Drain a small number of expired frames per iteration.
                        // Missing metadata is an intentional drop, not a reason
                        // to stall all later exact pairs.
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            auto decision = synchronizer.next(
                                std::chrono::steady_clock::now());

                            if (decision.type ==
                                reg::video::SyncDecisionType::None) {
                                break;
                            }

                            if (decision.type ==
                                reg::video::SyncDecisionType::
                                    DropMissingMetadata) {
                                overlayMissingMetadataDrops.fetch_add(
                                    1,
                                    std::memory_order_relaxed);
                                didWork = true;
                                continue;
                            }

                            if (decision.type ==
                                    reg::video::SyncDecisionType::Present &&
                                decision.frame) {
                                trackHistory.update(
                                    *decision.frame->metadata,
                                    std::chrono::steady_clock::now());

                                pendingOverlayFrame =
                                    std::move(*decision.frame);
                                break;
                            }
                        }
                    }

                    if (pendingOverlayFrame) {
                        const auto& synchronized =
                            *pendingOverlayFrame;

                        const auto extent =
                            overlayWindow->swapchain().extent();

                        reg::video::VideoTransform transform(
                            static_cast<std::uint32_t>(
                                synchronized.buffered.video->width()),
                            static_cast<std::uint32_t>(
                                synchronized.buffered.video->height()),
                            extent.width,
                            extent.height);

                        const reg::render::OverlayScene scene =
                            overlayBuilder.build(
                                *synchronized.metadata,
                                trackHistory,
                                transform);

                        overlaySceneRenderer->prepare(
                            scene,
                            overlayWindow->swapchain());

                        if (overlayRenderer->render(
                                synchronized.buffered.video,
                                overlayWindow->swapchain(),
                                overlaySceneRenderer.get())) {
                            const auto count =
                                overlayPresentedFrames.fetch_add(
                                    1,
                                    std::memory_order_relaxed) +
                                1;

                            if (count == 1 || count % 120 == 0) {
                                std::cout
                                    << "[overlay] presented="
                                    << count
                                    << " epoch="
                                    << synchronized.buffered.key.streamEpoch
                                    << " frame="
                                    << synchronized.buffered.key.frameId
                                    << " metadata_sequence="
                                    << synchronized.metadata->sequence
                                    << '\n';
                            }

                            pendingOverlayFrame.reset();
                            didWork = true;
                        }
                    }
                }

                if (!didWork) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
        } catch (...) {
            stopAndJoin();
            throw;
        }

        stopAndJoin();

        {
            std::scoped_lock lock(errorMutex);
            if (metadataError) {
                std::rethrow_exception(metadataError);
            }
        }

        std::cout
            << "[probe] RTSP sessions="
            << decoderSessionGeneration.load(
                std::memory_order_relaxed)
            << " reconnects="
            << decoderReconnects.load(
                std::memory_order_relaxed)
            << " connected="
            << (decoderConnected.load(
                    std::memory_order_relaxed)
                    ? "yes"
                    : "no")
            << '\n';

        {
            std::scoped_lock lock(errorMutex);
            if (!lastDecoderError.empty()) {
                std::cout
                    << "[probe] last RTSP error: "
                    << lastDecoderError
                    << '\n';
            }
        }

        std::cout
            << "[probe] decoded Vulkan frames: "
            << decodedFrames.load(std::memory_order_relaxed)
            << '\n';
        std::cout
            << "[probe] raw presented frames: "
            << rawPresentedFrames.load(std::memory_order_relaxed)
            << '\n';

        if (blackboxRecorder) {
            const auto recorderStats =
                blackboxRecorder->stats();

            std::cout
                << "[probe] recorder accepted="
                << recorderStats.packetsAccepted
                << " written="
                << recorderStats.packetsWritten
                << " queue_drops="
                << recorderStats.packetsDroppedQueueFull
                << " waiting_keyframe="
                << recorderStats.packetsWaitingForKeyframe
                << " rotations="
                << recorderStats.segmentRotations
                << " metadata_accepted="
                << recorderStats.metadataAccepted
                << " metadata_written="
                << recorderStats.metadataWritten
                << " metadata_queue_drops="
                << recorderStats.metadataDroppedQueueFull
                << " failures="
                << recorderStats.failures
                << '\n';

            if (recorderStats.failed) {
                std::cerr
                    << "[recorder] degraded: "
                    << blackboxRecorder->lastError()
                    << '\n';
            }
        }

        if (options.overlayEnabled) {
            const auto receiverStats = metadataReceiver->stats();

            std::cout
                << "[probe] overlay presented frames: "
                << overlayPresentedFrames.load(
                    std::memory_order_relaxed)
                << '\n';
            std::cout
                << "[probe] overlay missing identity: "
                << overlayMissingIdentity.load(
                    std::memory_order_relaxed)
                << '\n';
            std::cout
                << "[probe] overlay buffer evictions: "
                << overlayBufferEvictions.load(
                    std::memory_order_relaxed)
                << '\n';
            std::cout
                << "[probe] overlay missing-metadata drops: "
                << overlayMissingMetadataDrops.load(
                    std::memory_order_relaxed)
                << '\n';
            std::cout
                << "[probe] metadata packets: "
                << receiverStats.packetsReceived
                << " invalid="
                << receiverStats.packetsInvalid
                << " packet_duplicates="
                << receiverStats.packetDuplicates
                << " out_of_order="
                << receiverStats.packetsOutOfOrder
                << " sequence_gaps="
                << receiverStats.sequenceGaps
                << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        reg::app::printUsage(
            argc > 0 ? argv[0] : "reg_probe");
        return 1;
    }
}
