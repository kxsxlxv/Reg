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
            "Reg - Vulkan RTSP / Exact Sync Probe",
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
        reg::video::OverlayFrameBuffer overlayFrames(
            std::chrono::milliseconds(options.overlayDelayMs),
            static_cast<std::size_t>(options.overlayMaxFrames));
        reg::metadata::MetadataStore metadataStore;
        reg::video::FrameSynchronizer synchronizer(
            overlayFrames,
            metadataStore);

        reg::vulkan::VideoRenderer renderer(vulkan);

        std::unique_ptr<reg::metadata::MetadataReceiver> metadataReceiver;
        if (options.metadataPort != 0) {
            metadataReceiver =
                std::make_unique<reg::metadata::MetadataReceiver>(
                    metadataStore,
                    reg::metadata::MetadataReceiverConfig{
                        .bindAddress = options.metadataBind,
                        .port = static_cast<std::uint16_t>(options.metadataPort),
                    });

            std::cout
                << "[metadata] listening on "
                << options.metadataBind
                << ':'
                << metadataReceiver->localPort()
                << " overlay_delay_ms="
                << options.overlayDelayMs
                << " overlay_max_frames="
                << options.overlayMaxFrames
                << '\n';
        }

        std::atomic_uint64_t decodedFrames{0};
        std::atomic_uint64_t presentedFrames{0};
        std::atomic_uint64_t overlayFramesMissingIdentity{0};
        std::atomic_uint64_t overlayBufferEvictions{0};

        std::atomic_bool decoderFinished{false};
        std::atomic_bool metadataFinished{false};

        std::mutex errorMutex;
        std::exception_ptr decoderError;
        std::exception_ptr metadataError;

        std::jthread decodeThread([&] {
            try {
                decoder.run([&](reg::video::VideoFramePtr frame) {
                    rawMailbox.publish(frame);

                    if (metadataReceiver) {
                        const auto pushResult = overlayFrames.push(frame);
                        if (pushResult ==
                            reg::video::OverlayPushResult::MissingIdentity) {
                            const auto missing =
                                overlayFramesMissingIdentity.fetch_add(
                                    1,
                                    std::memory_order_relaxed) + 1;

                            if (missing == 1) {
                                std::cerr
                                    << "[overlay-sync] decoded frames have no Reg SEI FrameKey; "
                                       "exact synchronization is inactive until the source "
                                       "embeds stream_epoch/frame_id\n";
                            }
                        } else if (
                            pushResult ==
                            reg::video::OverlayPushResult::EvictedOldestAndAccepted) {
                            overlayBufferEvictions.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                    }

                    const auto count =
                        decodedFrames.fetch_add(
                            1,
                            std::memory_order_relaxed) + 1;

                    if (count == 1 || count % 120 == 0) {
                        std::cout
                            << "[decoder] frame="
                            << count
                            << " size="
                            << frame->width()
                            << 'x'
                            << frame->height();

                        if (const auto identity = frame->identity()) {
                            std::cout
                                << " epoch="
                                << identity->key.streamEpoch
                                << " source_frame="
                                << identity->key.frameId;
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

        const auto stopWorkersAndJoin = [&] {
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
        std::uint64_t synchronizedFrames = 0;
        std::uint64_t droppedOverlayFrames = 0;

        try {
            while (!decoderFinished.load(std::memory_order_acquire)) {
                if (platform.pollQuitRequested()) {
                    decoder.requestStop();
                    if (metadataReceiver) {
                        metadataReceiver->requestStop();
                    }
                    break;
                }

                if (metadataReceiver &&
                    metadataFinished.load(std::memory_order_acquire)) {
                    std::scoped_lock lock(errorMutex);
                    if (metadataError) {
                        std::rethrow_exception(metadataError);
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
                                << "[renderer] presented="
                                << count
                                << " source="
                                << latest->width()
                                << 'x'
                                << latest->height()
                                << " output="
                                << swapchain.extent().width
                                << 'x'
                                << swapchain.extent().height
                                << '\n';
                        }
                    } else {
                        std::this_thread::yield();
                    }
                } else {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }

                if (metadataReceiver) {
                    while (true) {
                        const auto decision =
                            synchronizer.next(
                                std::chrono::steady_clock::now());

                        if (decision.action ==
                            reg::video::SyncAction::Wait) {
                            break;
                        }

                        if (decision.action ==
                            reg::video::SyncAction::DropMissingMetadata) {
                            ++droppedOverlayFrames;
                            if (droppedOverlayFrames == 1 ||
                                droppedOverlayFrames % 120 == 0) {
                                std::cout
                                    << "[overlay-sync] dropped_missing_metadata="
                                    << droppedOverlayFrames
                                    << " epoch="
                                    << decision.key.streamEpoch
                                    << " frame="
                                    << decision.key.frameId
                                    << '\n';
                            }
                            continue;
                        }

                        ++synchronizedFrames;
                        if (synchronizedFrames == 1 ||
                            synchronizedFrames % 120 == 0) {
                            const auto& metadata =
                                *decision.frame.metadata;
                            std::cout
                                << "[overlay-sync] matched="
                                << synchronizedFrames
                                << " epoch="
                                << decision.key.streamEpoch
                                << " frame="
                                << decision.key.frameId
                                << " targets="
                                << metadata.targets.size()
                                << " cv_us="
                                << (
                                    metadata.cvEndNs >= metadata.cvBeginNs
                                        ? (metadata.cvEndNs - metadata.cvBeginNs) / 1000
                                        : 0)
                                << '\n';
                        }
                    }
                }
            }
        } catch (...) {
            stopWorkersAndJoin();
            throw;
        }

        stopWorkersAndJoin();

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

        if (metadataReceiver) {
            const auto receiverStats = metadataReceiver->stats();
            const auto syncStats = synchronizer.stats();

            std::cout
                << "[probe] metadata datagrams: "
                << receiverStats.receivedDatagrams
                << " valid="
                << receiverStats.validPackets
                << " invalid="
                << receiverStats.invalidPackets
                << " duplicate="
                << receiverStats.duplicatePackets
                << " out_of_order="
                << receiverStats.outOfOrderPackets
                << " gaps_observed="
                << receiverStats.sequenceGapsObserved
                << '\n';

            std::cout
                << "[probe] overlay sync presented="
                << syncStats.presented
                << " dropped_missing_metadata="
                << syncStats.droppedMissingMetadata
                << " missing_identity="
                << overlayFramesMissingIdentity.load(
                    std::memory_order_relaxed)
                << " buffer_evictions="
                << overlayBufferEvictions.load(
                    std::memory_order_relaxed)
                << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        reg::app::printUsage(argc > 0 ? argv[0] : "reg_probe");
        return 1;
    }
}
