#pragma once

#include "media/FrameIdentity.hpp"

#include <cstdint>
#include <vector>

namespace reg::media {

enum class H264NalFraming {
    AnnexB,
    Avcc4ByteLength,
};

// Builds one H.264 SEI NAL unit containing user_data_unregistered with the
// project UUID + canonical 32-byte FrameIdentity payload.
//
// AnnexB output:
//   00 00 00 01 | nal_header(type=6) | EBSP
//
// AVCC output:
//   big-endian u32 nal_size | nal_header(type=6) | EBSP
//
// The caller inserts this NAL in the same access unit as the corresponding
// source frame, before the frame's VCL NAL units.
std::vector<std::uint8_t> buildFrameIdentitySeiNal(
    const SourceFrameIdentity& identity,
    H264NalFraming framing);

} // namespace reg::media
