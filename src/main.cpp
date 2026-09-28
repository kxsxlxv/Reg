#include "app/CommandLine.hpp"
#include "media/RtspDecoder.hpp"
#include "media/VulkanHwDevice.hpp"
#include "metadata/MetadataReceiver.hpp"
#include "metadata/MetadataStore.hpp"
#include "metadata/TrackHistory.hpp"
#include "platform/SDLPlatform.hpp"
#include "render/ImGuiOverlayRenderer.hpp"
#include "render/ImGuiTelemetryRenderer.hpp"
#include "render/TargetOverlayBuilder.hpp"
#include "recorder/BlackboxRecorder.hpp"
#include "telemetry/TelemetryModel.hpp"
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
#include <condition_variable>
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
        std::unique_ptr<reg::vulkan::RenderWindow> telemetryWindow;

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

        if (options.telemetryEnabled) {
            SDL_Window* telemetrySdlWindow =
                platform.createVulkanWindow(
                    "Reg - Telemetry",
                    1280,
                    720);

            telemetryWindow =
                std::make_unique<reg::vulkan::RenderWindow>(
                    vulkan,
                    telemetrySdlWindow,
                    reg::vulkan::PresentPolicy::
                        LowLatencyTearingAllowed);
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
        std::unique_ptr<reg::render::ImGuiTelemetryRenderer>
            telemetryRenderer;

        reg::telemetry::TelemetryModel telemetryModel(
            std::chrono::minutes{5},
            std::chrono::milliseconds{250},
            4096);
        telemetryModel.log(
            reg::telemetry::Severity::Info,
            "Application started");

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

        if (options.telemetryEnabled) {
            telemetryRenderer =
                std::make_unique<
                    reg::render::ImGuiTelemetryRenderer>(
                        vulkan,
                        telemetryWindow->swapchain());
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
        std::atomic_uint64_t decoderCleanupRequest{0};
        std::atomic_uint64_t decoderCleanupAck{0};

        std::mutex cleanupMutex;
        std::condition_variable cleanupCondition;

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

                                    telemetryModel.log(
                                        reg::telemetry::Severity::Info,
                                        "RTSP session opened");

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

                    telemetryModel.log(
                        reg::telemetry::Severity::Warning,
                        "RTSP decoder session ended unexpectedly");
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

                    telemetryModel.log(
                        reg::telemetry::Severity::Warning,
                        std::string("RTSP session failed: ") +
                            error.what());

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

                    telemetryModel.log(
                        reg::telemetry::Severity::Error,
                        "RTSP session failed: unknown error");

                    std::cerr
                        << "[watchdog] RTSP session failed: unknown error\n";
                }

                if (shutdownRequested.load(
                        std::memory_order_acquire)) {
                    break;
                }

                if (sessionOpened) {
                    const std::uint64_t cleanupToken =
                        decoderCleanupRequest.fetch_add(
                            1,
                            std::memory_order_acq_rel) +
                        1;

                    std::unique_lock cleanupLock(
                        cleanupMutex);

                    cleanupCondition.wait(
                        cleanupLock,
                        [&] {
                            return shutdownRequested.load(
                                       std::memory_order_acquire) ||
                                   decoderCleanupAck.load(
                                       std::memory_order_acquire) >=
                                       cleanupToken;
                        });

                    if (shutdownRequested.load(
                            std::memory_order_acquire)) {
                        break;
                    }
                }

                const auto reconnectNumber =
                    decoderReconnects.fetch_add(
                        1,
                        std::memory_order_relaxed) +
                    1;

                telemetryModel.log(
                    reg::telemetry::Severity::Info,
                    std::string("RTSP reconnect attempt ") +
                        std::to_string(reconnectNumber) +
                        " after " +
                        std::to_string(reconnectDelayMs) +
                        " ms");

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
                    const std::exception_ptr error =
                        std::current_exception();

                    {
                        std::scoped_lock lock(errorMutex);
                        metadataError = error;
                    }

                    std::string message =
                        "Metadata receiver failed";

                    try {
                        if (error) {
                            std::rethrow_exception(error);
                        }
                    } catch (const std::exception& exception) {
                        message += ": ";
                        message += exception.what();
                    } catch (...) {
                        message += ": unknown error";
                    }

                    telemetryModel.log(
                        reg::telemetry::Severity::Error,
                        std::move(message));

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
            cleanupCondition.notify_all();

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
        bool recorderFailureLogged{false};
        auto nextTelemetryRenderAt =
            std::chrono::steady_clock::now();

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

                const std::uint64_t cleanupRequest =
                    decoderCleanupRequest.load(
                        std::memory_order_acquire);

                if (cleanupRequest >
                    decoderCleanupAck.load(
                        std::memory_order_acquire)) {
                    // No new decoder session is allowed to open until this
                    // render-thread cleanup is acknowledged.
                    rawMailbox.clear();
                    overlayFrames.clear();
                    metadataStore.clear();

                    pendingOverlayFrame.reset();
                    lastRawPresented.reset();
                    trackHistory.clear();
                    telemetryModel.clearTargets();
                    telemetryModel.log(
                        reg::telemetry::Severity::Info,
                        "Retiring Vulkan video session resources");

                    rawRenderer.resetVideoSession();
                    if (overlayRenderer) {
                        overlayRenderer->resetVideoSession();
                    }

                    decoderCleanupAck.store(
                        cleanupRequest,
                        std::memory_order_release);
                    cleanupCondition.notify_all();
                    didWork = true;
                }

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
                    telemetryModel.clearTargets();
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

                                telemetryModel.setTargets(
                                    *decision.frame->metadata);

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

                const auto telemetryNow =
                    std::chrono::steady_clock::now();

                if (options.telemetryEnabled &&
                    telemetryWindow &&
                    telemetryRenderer &&
                    telemetryNow >= nextTelemetryRenderAt) {
                    reg::metadata::MetadataReceiverStats
                        receiverStats{};

                    if (metadataReceiver) {
                        receiverStats =
                            metadataReceiver->stats();
                    }

                    reg::recorder::BlackboxRecorderStats
                        recorderStats{};

                    if (blackboxRecorder) {
                        recorderStats =
                            blackboxRecorder->stats();

                        if (recorderStats.failed &&
                            !recorderFailureLogged) {
                            recorderFailureLogged = true;
                            telemetryModel.log(
                                reg::telemetry::Severity::Error,
                                std::string("Blackbox recorder degraded: ") +
                                    blackboxRecorder->lastError());
                        }
                    }

                    telemetryModel.updateCounters(
                        reg::telemetry::Counters{
                            .rtspConnected =
                                decoderConnected.load(
                                    std::memory_order_relaxed),
                            .decoderSessions =
                                decoderSessionGeneration.load(
                                    std::memory_order_relaxed),
                            .reconnects =
                                decoderReconnects.load(
                                    std::memory_order_relaxed),
                            .decodedFrames =
                                decodedFrames.load(
                                    std::memory_order_relaxed),
                            .rawPresentedFrames =
                                rawPresentedFrames.load(
                                    std::memory_order_relaxed),
                            .overlayPresentedFrames =
                                overlayPresentedFrames.load(
                                    std::memory_order_relaxed),
                            .overlayMissingMetadataDrops =
                                overlayMissingMetadataDrops.load(
                                    std::memory_order_relaxed),
                            .metadataPackets =
                                receiverStats.packetsReceived,
                            .metadataInvalid =
                                receiverStats.packetsInvalid,
                            .metadataDuplicates =
                                receiverStats.packetDuplicates +
                                receiverStats.frameDuplicates,
                            .metadataSequenceGaps =
                                receiverStats.sequenceGaps,
                            .recorderPacketsWritten =
                                recorderStats.packetsWritten,
                            .recorderQueueDrops =
                                recorderStats.packetsDroppedQueueFull,
                            .recorderMetadataWritten =
                                recorderStats.metadataWritten,
                            .recorderMetadataQueueDrops =
                                recorderStats.metadataDroppedQueueFull,
                            .recorderFailures =
                                recorderStats.failures,
                            .overlayBufferDepth =
                                overlayFrames.size(),
                            .metadataStoreDepth =
                                metadataStore.size(),
                            .recorderQueueDepth =
                                recorderStats.queueDepth,
                        },
                        telemetryNow);

                    const auto telemetrySnapshot =
                        telemetryModel.snapshot();

                    if (telemetryRenderer->render(
                            telemetrySnapshot,
                            telemetryWindow->swapchain())) {
                        didWork = true;
                    }

                    nextTelemetryRenderAt =
                        telemetryNow +
                        std::chrono::milliseconds{100};
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
