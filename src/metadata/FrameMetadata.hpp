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
};

struct TargetMetadata {
    std::uint64_t id{};
    std::uint16_t classId{};
    std::uint16_t flags{};
    float confidence{};
    NormalizedRect bbox{};
};

struct FrameMetadata {
    media::FrameKey key{};
    std::uint32_t packetSequence{};
    std::uint16_t flags{};
    std::uint64_t cvBeginNs{};
    std::uint64_t cvEndNs{};
    std::chrono::steady_clock::time_point receivedAt{};
    std::vector<TargetMetadata> targets;
};

using FrameMetadataPtr = std::shared_ptr<const FrameMetadata>;

} // namespace reg::metadata
