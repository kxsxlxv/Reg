#pragma once

#include "media/FrameIdentity.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace reg::source {

// Generates a non-zero 64-bit stream epoch suitable for one logical source
// encoder session. A new session must create a new epoch.
std::uint64_t generateStreamEpoch();

// Inserts the canonical Reg H.264 user_data_unregistered SEI into an Annex-B
// access unit immediately before the first VCL NAL unit.
//
// Existing prefix NALs (for example AUD/SPS/PPS/other SEI) retain their order.
// The input and output buffers must not alias.
//
// Throws std::invalid_argument when the input is not a usable Annex-B access
// unit containing a VCL NAL.
void injectFrameIdentitySeiIntoAnnexBAccessUnit(
    const media::SourceFrameIdentity& identity,
    std::span<const std::uint8_t> encodedAccessUnit,
    std::vector<std::uint8_t>& output);

class FrameIdentityAccessUnitInjector final {
public:
    explicit FrameIdentityAccessUnitInjector(
        std::uint64_t streamEpoch = generateStreamEpoch(),
        std::uint64_t firstFrameId = 0);

    // Assigns the next source frame_id, injects the corresponding Reg SEI into
    // this encoded Annex-B access unit, and returns the exact identity used.
    //
    // frame_id advances only after successful injection.
    media::SourceFrameIdentity injectNext(
        std::span<const std::uint8_t> encodedAccessUnit,
        std::uint64_t sourceTimeNs,
        std::vector<std::uint8_t>& output);

    std::uint64_t streamEpoch() const noexcept {
        return streamEpoch_;
    }

    std::uint64_t nextFrameId() const noexcept {
        return nextFrameId_;
    }

private:
    std::uint64_t streamEpoch_{};
    std::uint64_t nextFrameId_{};
};

} // namespace reg::source
