#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace reg::media {

// An FFmpeg interrupt callback may run on a different thread from the
// decoder loop. A steady-clock deadline is independent of wall-clock jumps.
// Zero denotes "disarmed" and is never an active deadline.
class IoDeadline final {
public:
    using Clock = std::chrono::steady_clock;

    void armUntil(Clock::time_point deadline) noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
        deadlineNs_.store(ns, std::memory_order_release);
    }

    void disarm() noexcept {
        deadlineNs_.store(0, std::memory_order_release);
    }

    bool expired(Clock::time_point now) const noexcept {
        const std::int64_t deadlineNs =
            deadlineNs_.load(std::memory_order_acquire);
        if (deadlineNs == 0) {
            return false;
        }
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        return nowNs >= deadlineNs;
    }

    bool expired() const noexcept {
        return expired(Clock::now());
    }

private:
    std::atomic<std::int64_t> deadlineNs_{0};
};

} // namespace reg::media
