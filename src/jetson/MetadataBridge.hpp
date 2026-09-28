#pragma once

#include "media/FrameIdentity.hpp"
#include "metadata/FrameMetadata.hpp"
#include "metadata/MetadataSender.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

struct AVFrame;

namespace reg::jetson {

class FrameContext final {
public:
    const media::FrameKey& key() const noexcept {
        return identity_.key;
    }

    std::uint64_t sourceTimeNs() const noexcept {
        return identity_.sourceTimeNs;
    }

    std::uint64_t cvBeginNs() const noexcept {
        return cvBeginNs_;
    }

private:
    friend class MetadataBridge;

    FrameContext(
        media::SourceFrameIdentity identity,
        std::uint64_t cvBeginNs) noexcept
        : identity_(identity),
          cvBeginNs_(cvBeginNs) {}

    media::SourceFrameIdentity identity_{};
    std::uint64_t cvBeginNs_{};
};

struct MetadataBridgeStats {
    std::uint64_t decodedFramesObserved{};
    std::uint64_t framesMissingIdentity{};
    metadata::MetadataSenderStats transport{};
};

class MetadataBridge final {
public:
    MetadataBridge(
        std::string_view remoteAddress,
        std::uint16_t remotePort,
        std::uint32_t initialSequence = 0);

    // Captures the source-assigned FrameIdentity from the decoded frame before
    // the AVFrame may be released or recycled. A missing/foreign/malformed Reg
    // SEI returns nullopt; callers must not substitute a local frame counter.
    std::optional<FrameContext> beginFrame(
        const AVFrame* decodedFrame,
        std::uint64_t cvBeginNs);

    // Sends one completed CV result using the exact FrameKey captured by
    // beginFrame(). Any key/timestamps already present in metadata are
    // overwritten so correspondence cannot accidentally come from a Jetson
    // local counter or an unrelated result object.
    bool sendResult(
        const FrameContext& context,
        std::uint64_t cvEndNs,
        metadata::FrameMetadata metadata);

    MetadataBridgeStats stats() const noexcept;

private:
    metadata::MetadataSender sender_;
    std::uint64_t decodedFramesObserved_{};
    std::uint64_t framesMissingIdentity_{};
};

} // namespace reg::jetson
