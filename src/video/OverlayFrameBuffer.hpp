#pragma once

#include "media/FrameIdentity.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
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

    const Entry* front() const noexcept {
        return frames_.empty() ? nullptr : &frames_.front();
    }

    Entry popFront() {
        if (frames_.empty()) {
            throw std::logic_error("OverlayFrameBuffer::popFront on empty buffer");
        }

        Entry result = std::move(frames_.front());
        frames_.pop_front();
        return result;
    }

    void clear() noexcept {
        frames_.clear();
    }

    void setPlayoutDelay(std::chrono::milliseconds delay) {
        if (delay.count() < 0) {
            throw std::invalid_argument("Overlay playout delay must be non-negative");
        }

        const auto delta = delay - playoutDelay_;
        playoutDelay_ = delay;

        // Existing frames retain their relative arrival time but follow the new
        // configured playout target. This is deterministic and will support a
        // future adaptive-delay controller without reconstructing the queue.
        for (auto& frame : frames_) {
            frame.deadline += delta;
        }
    }

    std::chrono::milliseconds playoutDelay() const noexcept {
        return playoutDelay_;
    }

    std::size_t size() const noexcept {
        return frames_.size();
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    std::chrono::milliseconds playoutDelay_{};
    std::size_t capacity_{};
    std::deque<Entry> frames_;
};

} // namespace reg::video
