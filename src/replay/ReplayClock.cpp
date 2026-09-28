#include "replay/ReplayClock.hpp"

#include <cmath>
#include <stdexcept>

namespace reg::replay {

ReplayClock::ReplayClock(double speed)
    : speed_(speed) {
    if (!std::isfinite(speed_) ||
        speed_ <= 0.0) {
        throw std::invalid_argument(
            "replay speed must be finite and positive");
    }
}

void ReplayClock::reset() noexcept {
    firstMediaTime_.reset();
    wallStart_ = {};
}

std::chrono::steady_clock::time_point
ReplayClock::targetTime(
    std::chrono::nanoseconds mediaTime,
    std::chrono::steady_clock::time_point now) {
    if (!firstMediaTime_) {
        firstMediaTime_ = mediaTime;
        wallStart_ = now;
        return wallStart_;
    }

    const auto mediaDelta =
        mediaTime - *firstMediaTime_;

    const auto scaled =
        std::chrono::duration<double>(
            mediaDelta) /
        speed_;

    return wallStart_ +
        std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                scaled);
}

} // namespace reg::replay
