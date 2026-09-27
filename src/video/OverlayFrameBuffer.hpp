#pragma once

#include "media/FrameIdentity.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace reg::video {

template <typename Payload>
class OverlayFrameBuffer final {
public:
    using PayloadPtr = std::shared_ptr<const Payload>;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Entry {
        media::FrameKey key{};
        PayloadPtr payload;
        TimePoint receivedAt{};
        TimePoint deadline{};
    };

    enum class PushResult {
        Inserted,
        EvictedOldestAndInserted,
    };

    OverlayFrameBuffer(
        std::chrono::milliseconds playoutDelay,
        std::size_t capacity)
        : playoutDelay_(playoutDelay),
          capacity_(capacity) {
        if (playoutDelay_.count() < 0) {
            throw std::invalid_argument("Overlay playout delay must be non-negative");
        }
        if (capacity_ == 0) {
            throw std::invalid_argument("Overlay frame-buffer capacity must be greater than zero");
        }
    }

    PushResult push(
        media::FrameKey key,
        PayloadPtr payload,
        TimePoint receivedAt) {
        if (!payload) {
            throw std::invalid_argument("OverlayFrameBuffer::push received null payload");
        }

        std::scoped_lock lock(mutex_);

        bool evicted = false;
        if (frames_.size() >= capacity_) {
            frames_.pop_front();
            evicted = true;
        }

        frames_.push_back(Entry{
            .key = key,
            .payload = std::move(payload),
            .receivedAt = receivedAt,
            .deadline = receivedAt + playoutDelay_,
        });

        return evicted
            ? PushResult::EvictedOldestAndInserted
            : PushResult::Inserted;
    }

    // Atomically checks the front deadline and removes the frame only if it is
    // ready for a synchronization decision. This avoids a race with producer
    // eviction when decoder and renderer run on different threads.
    std::optional<Entry> popReady(TimePoint now) {
        std::scoped_lock lock(mutex_);
        if (frames_.empty() || now < frames_.front().deadline) {
            return std::nullopt;
        }

        Entry result = std::move(frames_.front());
        frames_.pop_front();
        return result;
    }

    std::optional<Entry> peekFront() const {
        std::scoped_lock lock(mutex_);
        if (frames_.empty()) {
            return std::nullopt;
        }
        return frames_.front();
    }

    void clear() {
        std::scoped_lock lock(mutex_);
        frames_.clear();
    }

    void setPlayoutDelay(std::chrono::milliseconds delay) {
        if (delay.count() < 0) {
            throw std::invalid_argument("Overlay playout delay must be non-negative");
        }

        std::scoped_lock lock(mutex_);
        const auto delta = delay - playoutDelay_;
        playoutDelay_ = delay;

        for (auto& frame : frames_) {
            frame.deadline += delta;
        }
    }

    std::chrono::milliseconds playoutDelay() const {
        std::scoped_lock lock(mutex_);
        return playoutDelay_;
    }

    std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return frames_.size();
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    mutable std::mutex mutex_;
    std::chrono::milliseconds playoutDelay_{};
    const std::size_t capacity_{};
    std::deque<Entry> frames_;
};

} // namespace reg::video
