#pragma once

#include "media/GtSei.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace reg::media {

// Host-only scalar map bytes. No Vulkan surface pointer or decoder frame lease.
enum class GtMapDecodeError : std::uint8_t {
    None, NotEncoded, Bounds, Codec, Size, Checksum, Pixels, Allocation,
};
struct GtDecodedMap {
    GtMapStatus status{GtMapStatus::Missing};
    GtMapDecodeError error{GtMapDecodeError::None};
    GtMapKind kind{GtMapKind::Sky};
    std::uint32_t width{}, height{};
    std::shared_ptr<const std::vector<std::uint8_t>> raw;
    std::uint64_t durationUs{};
    // Keep metric metres as little-endian float32; never colourise canonical GT.
};
[[nodiscard]] GtDecodedMap decodeGtMap(const GtMapMetadata& encoded) noexcept;

} // namespace reg::media
