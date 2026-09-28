#pragma once

#include "source/FrameIdentityAccessUnitInjector.hpp"

#include <UnigineCallback.h>
#include <plugins/Unigine/RTSPStreamer/UnigineRTSPStreamer.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace reg::integration::unigine {

struct RtspFrameIdentityBridgeStats {
    std::uint64_t encodedFramesObserved{};
    std::uint64_t accessUnitsInjected{};
    std::uint64_t publisherQueueDrops{};
    std::uint64_t failures{};
};

class RtspFrameIdentityBridge final {
public:
    using Streamer =
        Unigine::Plugins::RTSPStreamer;

    // This callback executes on RTSPStreamer's encoder thread.
    //
    // It must synchronously copy/move the compressed bytes into a bounded
    // non-blocking publisher queue if it needs to retain them after returning.
    // It must never perform blocking network/disk work.
    using TrySubmitCallback =
        std::function<bool(
            std::span<const std::uint8_t> accessUnit,
            unsigned long long presentationTimestamp,
            const media::SourceFrameIdentity& identity)>;

    RtspFrameIdentityBridge(
        Streamer* streamer,
        Streamer::StreamHandle stream,
        TrySubmitCallback trySubmit,
        std::uint64_t streamEpoch =
            source::generateStreamEpoch())
        : streamer_(streamer),
          stream_(stream),
          trySubmit_(std::move(trySubmit)),
          injector_(streamEpoch) {
        if (streamer_ == nullptr) {
            throw std::invalid_argument(
                "RTSPStreamer bridge requires streamer");
        }

        if (!trySubmit_) {
            throw std::invalid_argument(
                "RTSPStreamer bridge requires publisher queue callback");
        }
    }

    ~RtspFrameIdentityBridge() {
        detach();
    }

    RtspFrameIdentityBridge(
        const RtspFrameIdentityBridge&) = delete;
    RtspFrameIdentityBridge& operator=(
        const RtspFrameIdentityBridge&) = delete;

    bool attach() {
        if (callback_.value != 0) {
            return true;
        }

        if (!streamer_->isInitialized()) {
            return false;
        }

        callback_ =
            streamer_->addStreamFrameEncodedCallback(
                stream_,
                Unigine::MakeCallback(
                    this,
                    &RtspFrameIdentityBridge::
                        onEncodedFrame));

        return callback_.value != 0;
    }

    void detach() {
        if (streamer_ != nullptr &&
            callback_.value != 0) {
            streamer_->
                removeStreamFrameEncodedCallback(
                    stream_,
                    callback_);
        }

        callback_ = {};
    }

    std::uint64_t streamEpoch() const noexcept {
        return injector_.streamEpoch();
    }

    RtspFrameIdentityBridgeStats
    stats() const noexcept {
        return RtspFrameIdentityBridgeStats{
            .encodedFramesObserved =
                encodedFramesObserved_.load(
                    std::memory_order_relaxed),
            .accessUnitsInjected =
                accessUnitsInjected_.load(
                    std::memory_order_relaxed),
            .publisherQueueDrops =
                publisherQueueDrops_.load(
                    std::memory_order_relaxed),
            .failures =
                failures_.load(
                    std::memory_order_relaxed),
        };
    }

private:
    static std::uint64_t
    monotonicNowNs() noexcept {
        const auto value =
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::
                        now().time_since_epoch())
                .count();

        if (value <= 0) {
            return 0;
        }

        return static_cast<std::uint64_t>(
            value);
    }

    void onEncodedFrame(
        const char* data,
        unsigned int size,
        unsigned long long presentationTimestamp) {
        encodedFramesObserved_.fetch_add(
            1,
            std::memory_order_relaxed);

        try {
            if (data == nullptr ||
                size == 0U) {
                failures_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return;
            }

            const std::span<
                const std::uint8_t>
                encoded{
                    reinterpret_cast<
                        const std::uint8_t*>(
                            data),
                    static_cast<std::size_t>(
                        size)};

            const auto identity =
                injector_.injectNext(
                    encoded,
                    monotonicNowNs(),
                    augmented_);

            accessUnitsInjected_.fetch_add(
                1,
                std::memory_order_relaxed);

            const bool accepted =
                trySubmit_(
                    std::span<
                        const std::uint8_t>{
                        augmented_},
                    presentationTimestamp,
                    identity);

            if (!accepted) {
                publisherQueueDrops_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        } catch (...) {
            // Never propagate application failures through UNIGINE's encoder
            // callback. A failed access unit is dropped and surfaced in stats.
            failures_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }

    Streamer* streamer_{};
    Streamer::StreamHandle stream_{};
    Streamer::FrameCallbackHandle callback_{};

    TrySubmitCallback trySubmit_;
    source::FrameIdentityAccessUnitInjector
        injector_;
    std::vector<std::uint8_t> augmented_;

    std::atomic_uint64_t
        encodedFramesObserved_{0};
    std::atomic_uint64_t
        accessUnitsInjected_{0};
    std::atomic_uint64_t
        publisherQueueDrops_{0};
    std::atomic_uint64_t
        failures_{0};
};

} // namespace reg::integration::unigine
