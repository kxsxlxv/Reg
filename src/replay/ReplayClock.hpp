#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace reg::replay {

class ReplayClock final {
public:
    explicit ReplayClock(double speed = 1.0);

    void reset() noexcept;

    std::chrono::steady_clock::time_point
    targetTime(
        std::chrono::nanoseconds mediaTime,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    double speed() const noexcept {
        return speed_;
    }

private:
    double speed_{1.0};

    std::optional<std::chrono::nanoseconds>
        firstMediaTime_;

    std::chrono::steady_clock::time_point
        wallStart_{};
};

} // namespace reg::replay
