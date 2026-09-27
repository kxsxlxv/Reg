#pragma once

#include <cstdint>

namespace reg::metadata {

enum class SequenceDisposition {
    First,
    InOrder,
    Gap,
    OutOfOrder,
    Duplicate,
    Stale,
};

struct SequenceObservation {
    SequenceDisposition disposition{SequenceDisposition::First};
    std::uint32_t missingBeforeThisPacket{};
};

class PacketSequenceTracker final {
public:
    SequenceObservation observe(std::uint32_t sequence) noexcept;
    void reset() noexcept;

    bool initialized() const noexcept { return initialized_; }
    std::uint32_t highestSequence() const noexcept { return highestSequence_; }

private:
    static constexpr std::uint32_t kWindowBits = 64;

    bool initialized_{false};
    std::uint32_t highestSequence_{};
    std::uint64_t seenWindow_{};
};

} // namespace reg::metadata
