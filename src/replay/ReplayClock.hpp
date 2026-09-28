#pragma once

#include "media/FrameIdentity.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace reg::replay {

class ReplayClock final {
public:
    void reset() noexcept;

    std::chrono::steady_clock::time_point targetTime(
        const media::SourceFrameIdentity& identity,
        std::chrono::steady_clock::time_point now);

private:
    std::optional<std::uint64_t> epoch_;
    std::optional<std::uint64_t> sourceBaseNs_;
    std::chrono::steady_clock::time_point wallBase_{};
};

} // namespace reg::replay
