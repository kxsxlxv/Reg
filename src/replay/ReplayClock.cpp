#include "replay/ReplayClock.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace reg::replay {

void ReplayClock::reset() noexcept {
    epoch_.reset();
    sourceBaseNs_.reset();
    wallBase_ = {};
}

std::chrono::steady_clock::time_point
ReplayClock::targetTime(
    const media::SourceFrameIdentity& identity,
    std::chrono::steady_clock::time_point now) {
    const bool newEpoch =
        !epoch_ ||
        *epoch_ != identity.key.streamEpoch;

    const bool regressed =
        sourceBaseNs_ &&
        identity.sourceTimeNs < *sourceBaseNs_;

    if (newEpoch ||
        !sourceBaseNs_ ||
        regressed) {
        epoch_ = identity.key.streamEpoch;
        sourceBaseNs_ = identity.sourceTimeNs;
        wallBase_ = now;
        return now;
    }

    const std::uint64_t deltaNs =
        identity.sourceTimeNs - *sourceBaseNs_;

    constexpr std::uint64_t maxNs =
        static_cast<std::uint64_t>(
            std::chrono::nanoseconds::max().count());

    const auto safeDelta =
        std::chrono::nanoseconds{
            static_cast<std::int64_t>(
                deltaNs > maxNs
                    ? maxNs
                    : deltaNs)};

    return wallBase_ + safeDelta;
}

} // namespace reg::replay
