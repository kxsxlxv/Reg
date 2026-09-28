#pragma once

#include "media/FrameIdentity.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace reg::metadata {

struct NormalizedRect {
    float x{};
    float y{};
    float width{};
    float height{};

    auto operator<=>(const NormalizedRect&) const = default;
};

struct TargetMetadata {
    std::uint64_t id{};
    std::uint16_t classId{};
    std::uint16_t flags{};
    float confidence{};
    NormalizedRect bbox{};

    auto operator<=>(const TargetMetadata&) const = default;
};

struct FrameMetadata {
    media::FrameKey key{};
    std::uint32_t sequence{};
    std::uint16_t flags{};

    std::uint64_t cvBeginNs{};
    std::uint64_t cvEndNs{};

    std::vector<TargetMetadata> targets;
    std::chrono::steady_clock::time_point receivedAt{};

    auto operator<=>(const FrameMetadata&) const = default;
};

using FrameMetadataPtr = std::shared_ptr<const FrameMetadata>;

} // namespace reg::metadata
