#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace reg::video {

class SignalState final {
public:
    using Clock = std::chrono::steady_clock;

    static void markFrame() noexcept {
        lastFrameNs().store(
            nowNs(),
            std::memory_order_release);
        present().store(
            true,
            std::memory_order_release);
    }

    static void markNoSignal() noexcept {
        present().store(
            false,
            std::memory_order_release);
    }

    static bool recent(
        std::chrono::milliseconds maximumAge =
            std::chrono::milliseconds{750}) noexcept {
        if (!present().load(std::memory_order_acquire)) {
            return false;
        }

        const std::int64_t last =
            lastFrameNs().load(std::memory_order_acquire);
        if (last <= 0) {
            return false;
        }

        const auto age = std::chrono::nanoseconds{
            std::max<std::int64_t>(0, nowNs() - last)};
        return age <= maximumAge;
    }

private:
    static std::int64_t nowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
    }

    static std::atomic_bool& present() noexcept {
        static std::atomic_bool value{false};
        return value;
    }

    static std::atomic<std::int64_t>& lastFrameNs() noexcept {
        static std::atomic<std::int64_t> value{0};
        return value;
    }
};

} // namespace reg::video
