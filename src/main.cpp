#include "app/CommandLine.hpp"
#include "media/RtspDecoder.hpp"
#include "media/VulkanHwDevice.hpp"
#include "metadata/MetadataReceiver.hpp"
#include "metadata/MetadataStore.hpp"
#include "platform/SDLPlatform.hpp"
#include "video/FrameSynchronizer.hpp"
#include "video/OverlayFrameBuffer.hpp"
#include "video/RawFrameMailbox.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VideoRenderer.hpp"
#include "vulkan/VulkanContext.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

int main(int argc, char** argv) {
    try {
        const auto options = reg::app::parseCommandLine(argc, argv);

        reg::platform::SDLPlatform platform;
        SDL_Window* window = platform.createVulkanWindow(
            "Reg - Vulkan Decode / Frame Sync Probe",
            1280,
            720);

        reg::vulkan::VulkanContext vulkan(window, options.validation);
        reg::vulkan::Swapchain swapchain(vulkan, window);
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
        reg::vulkan::VideoRenderer renderer(vulkan);

        using OverlayBuffer =
            reg::video::OverlayFrameBuffer<reg::video::VideoFrame>;
        using OverlaySynchronizer =
            reg::video::FrameSynchronizer<reg::video::VideoFrame>;

        reg::metadata::MetadataStore metadataStore(options.metadataCapacity);
        OverlayBuffer overlayBuffer(
            std::chrono::milliseconds(options.overlayDelayMs),
            options.overlayBufferFrames);
        OverlaySynchronizer overlaySynchronizer(
            overlayBuffer,
            metadataStore);

        const bool metadataEnabled = options.metadataPort != 0;

        std::unique_ptr<reg::metadata::MetadataReceiver> metadataReceiver;
        if (metadataEnabled) {
            metadataReceiver =
                std::make_unique<reg::metadata::MetadataReceiver>(
                    metadataStore,
                    reg::metadata::MetadataReceiverConfig{
                        .bindAddress = options.metadataBind,
                        .port = options.metadataPort,
                        .pollTimeout = std::chrono::milliseconds(20),
                    });

            std::cout
                << "[metadata] enabled on "
                << options.metadataBind << ':' << options.metadataPort
                << " overlay_delay_ms=" << options.overlayDelayMs
                << " overlay_buffer_frames=" << options.overlayBufferFrames
                << '\n';
        }

        std::atomic_uint64_t decodedFrames{0};
        std::atomic_uint64_t presentedFrames{0};
        std::atomic_uint64_t framesWithoutIdentity{0};
        std::atomic_uint64_t overlayMatchedFrames{0};
        std::atomic_uint64_t overlayDroppedFrames{0};

        std::atomic_bool decoderFinished{false};
        std::atomic_bool metadataFinished{false};

        std::mutex errorMutex;
        std::exception_ptr decoderError;
        std::exception_ptr metadataError;

        std::jthread metadataThread;
        if (metadataReceiver) {
            metadataThread = std::jthread([&] {
                try {
                    metadataReceiver->run();
                } catch (...) {
                    std::scoped_lock lock(errorMutex);
                    metadataError = std::current_exception();
                }
                metadataFinished.store(true, std::memory_order_release);
            });
        }

        std::jthread decodeThread([&] {
            try {
                decoder.run([&](reg::video::VideoFramePtr frame) {
                    rawMailbox.publish(frame);

                    if (metadataEnabled) {
                        if (const auto identity = frame->identity()) {
                            overlaySynchronizer.pushFrame(
                                identity->key,
                                frame,
                                frame->decodedAt());
                        } else {
                            framesWithoutIdentity.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                    }

                    const auto count =
                        decodedFrames.fetch_add(
                            1,
                            std::memory_order_relaxed) + 1;

                    if (count == 1 || count % 120 == 0) {
                        std::cout << "[decoder] frame=" << count
                                  << " size=" << frame->width()
                                  << 'x' << frame->height();

                        if (const auto identity = frame->identity()) {
                            std::cout
                                << " epoch=" << identity->key.streamEpoch
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

        const auto stopAndJoin = [&] {
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
        };

        reg::video::VideoFramePtr lastPresented;

        try {
            while (!decoderFinished.load(std::memory_order_acquire)) {
                if (metadataEnabled &&
                    metadataFinished.load(std::memory_order_acquire)) {
                    break;
                }

                if (platform.pollQuitRequested()) {
                    break;
                }

                if (metadataEnabled) {
                    const auto syncResult =
                        overlaySynchronizer.next(
                            std::chrono::steady_clock::now());

                    if (syncResult.droppedFrames != 0) {
                        overlayDroppedFrames.fetch_add(
                            syncResult.droppedFrames,
                            std::memory_order_relaxed);
                    }

                    if (syncResult.action == reg::video::SyncAction::Present) {
                        const auto matched =
                            overlayMatchedFrames.fetch_add(
                                1,
                                std::memory_order_relaxed) + 1;

                        if (matched == 1 || matched % 60 == 0) {
                            const auto metadataLatency =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    syncResult.metadata->receivedAt -
                                    syncResult.frame->decodedAt());

                            std::cout
                                << "[overlay-sync] matched=" << matched
                                << " epoch=" << syncResult.key.streamEpoch
                                << " frame=" << syncResult.key.frameId
                                << " metadata_latency_ms="
                                << metadataLatency.count()
                                << " dropped_total="
                                << overlayDroppedFrames.load(
                                    std::memory_order_relaxed)
                                << '\n';
                        }
                    }
                }

                const auto latest = rawMailbox.latest();
                if (latest && latest != lastPresented) {
                    if (renderer.render(latest, swapchain)) {
                        lastPresented = latest;
                        const auto count =
                            presentedFrames.fetch_add(
                                1,
                                std::memory_order_relaxed) + 1;

                        if (count == 1 || count % 120 == 0) {
                            std::cout
                                << "[renderer] presented=" << count
                                << " source=" << latest->width()
                                << 'x' << latest->height()
                                << " output=" << swapchain.extent().width
                                << 'x' << swapchain.extent().height
                                << '\n';
                        }
                    } else {
                        std::this_thread::yield();
                    }
                } else {
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
            if (decoderError) {
                std::rethrow_exception(decoderError);
            }
            if (metadataError) {
                std::rethrow_exception(metadataError);
            }
        }

        std::cout
            << "[probe] decoded Vulkan frames: "
            << decodedFrames.load(std::memory_order_relaxed)
            << '\n';
        std::cout
            << "[probe] presented raw frames: "
            << presentedFrames.load(std::memory_order_relaxed)
            << '\n';

        if (metadataEnabled) {
            const auto stats = metadataReceiver->stats();

            std::cout
                << "[probe] overlay matched: "
                << overlayMatchedFrames.load(std::memory_order_relaxed)
                << " dropped: "
                << overlayDroppedFrames.load(std::memory_order_relaxed)
                << " missing_sei: "
                << framesWithoutIdentity.load(std::memory_order_relaxed)
                << '\n';

            std::cout
                << "[probe] metadata rx=" << stats.datagramsReceived
                << " accepted=" << stats.packetsAccepted
                << " invalid=" << stats.invalidPackets
                << " duplicate_seq=" << stats.duplicateSequences
                << " duplicate_frame=" << stats.duplicateFrames
                << " out_of_order=" << stats.outOfOrderSequences
                << " gaps_observed=" << stats.sequenceGapsObserved
                << " late=" << stats.latePackets
                << " store_evictions=" << stats.storeEvictions
                << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        reg::app::printUsage(argc > 0 ? argv[0] : "reg_probe");
        return 1;
    }
}
