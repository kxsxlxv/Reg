#pragma once

#include "media/FrameIdentity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct AVFrame;

namespace reg::media {

inline constexpr std::array<std::uint8_t, 16> kGtSeiUuid{
    0x62, 0x42, 0xb3, 0x58, 0xd3, 0x0d, 0x48, 0x4a,
    0x9e, 0xf7, 0x5e, 0x3f, 0x9c, 0x24, 0x1b, 0x07,
};
inline constexpr std::size_t kMaxGtJsonBytes = 16U * 1024U;

enum class GtStatus : std::uint8_t {
    Missing,
    Ready,
    NotReady,
    Busy,
    AmbiguousViewport,
    Unavailable,
    ParseError,
    FrameMismatch,
};

const char* gtStatusName(GtStatus status) noexcept;

struct GtObject {
    std::string id;
    bool visible{};
    // Continuous half-open [xmin,ymin,xmax,ymax] in video pixels.
    std::array<double, 4> xyxyVideo{};
    bool validBox{};
};

struct GtMetadata {
    GtStatus status{GtStatus::Missing};
    SourceFrameIdentity source{};
    std::uint32_t videoWidth{};
    std::uint32_t videoHeight{};
    std::vector<GtObject> objects;
};

// Does not require FFmpeg and never throws, including on malformed input.
// JSON integer fields are parsed directly to uint64_t, never via double.
GtMetadata parseGtJson(std::span<const std::uint8_t> json) noexcept;

// Iterates every AV_FRAME_DATA_SEI_UNREGISTERED side-data entry, ignores
// unrelated UUIDs, and rejects duplicate GT UUIDs. No packet-order pairing.
GtMetadata extractGtSei(const AVFrame* frame) noexcept;

// The frame's own RGF1 identity is the sole authority for GT association.
// A mismatch, missing identity, or unexpected video dimensions disables OSD.
void validateGtForFrame(
    GtMetadata& gt,
    const std::optional<SourceFrameIdentity>& rgf1,
    int decodedWidth,
    int decodedHeight) noexcept;

} // namespace reg::media
