#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>

struct AVFrame;

namespace reg::media {

struct FrameKey {
    std::uint64_t streamEpoch{};
    std::uint64_t frameId{};

    auto operator<=>(const FrameKey&) const = default;
};

struct SourceFrameIdentity {
    FrameKey key{};
    std::uint64_t sourceTimeNs{};
};

// Project-specific H.264/H.265 user_data_unregistered UUID.
// The encoder and Jetson decoder must use this exact UUID when Phase C is enabled.
inline constexpr std::array<std::uint8_t, 16> kFrameIdentitySeiUuid{
    0x7f, 0x53, 0x3b, 0x8d,
    0x1a, 0x91,
    0x4c, 0x2d,
    0x9f, 0x6a,
    0x52, 0x45,
    0x47, 0x46,
    0x49, 0x44,
};

std::optional<SourceFrameIdentity> extractFrameIdentity(const AVFrame* frame);

} // namespace reg::media
