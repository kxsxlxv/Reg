#include "metadata/PacketSequenceTracker.hpp"

#include <cstdint>

namespace reg::metadata {

SequenceObservation PacketSequenceTracker::observe(
    std::uint32_t sequence) noexcept {
    if (!initialized_) {
        initialized_ = true;
        highestSequence_ = sequence;
        seenWindow_ = 1ULL;
        return {
            .disposition = SequenceDisposition::First,
            .missingBeforeThisPacket = 0,
        };
    }

    const std::uint32_t forwardDistance = sequence - highestSequence_;
    if (forwardDistance == 0) {
        return {
            .disposition = SequenceDisposition::Duplicate,
            .missingBeforeThisPacket = 0,
        };
    }

    // Serial-number arithmetic: distances below half the uint32 range are
    // interpreted as forward movement. This remains correct across wrap-around.
    if (forwardDistance < 0x80000000U) {
        const std::uint32_t missing =
            forwardDistance > 0 ? forwardDistance - 1U : 0U;

        if (forwardDistance >= kWindowBits) {
            seenWindow_ = 1ULL;
        } else {
            seenWindow_ = (seenWindow_ << forwardDistance) | 1ULL;
        }

        highestSequence_ = sequence;
        return {
            .disposition =
                forwardDistance == 1U
                    ? SequenceDisposition::InOrder
                    : SequenceDisposition::Gap,
            .missingBeforeThisPacket = missing,
        };
    }

    const std::uint32_t backwardDistance = highestSequence_ - sequence;
    if (backwardDistance >= kWindowBits) {
        return {
            .disposition = SequenceDisposition::Stale,
            .missingBeforeThisPacket = 0,
        };
    }

    const std::uint64_t bit = 1ULL << backwardDistance;
    if ((seenWindow_ & bit) != 0) {
        return {
            .disposition = SequenceDisposition::Duplicate,
            .missingBeforeThisPacket = 0,
        };
    }

    seenWindow_ |= bit;
    return {
        .disposition = SequenceDisposition::OutOfOrder,
        .missingBeforeThisPacket = 0,
    };
}

void PacketSequenceTracker::reset() noexcept {
    initialized_ = false;
    highestSequence_ = 0;
    seenWindow_ = 0;
}

} // namespace reg::metadata
