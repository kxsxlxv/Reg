#pragma once

#include "metadata/MetadataStore.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

namespace reg::video {

struct SynchronizedFrame {
    BufferedOverlayFrame buffered{};
    metadata::FrameMetadataPtr metadata{};
};

enum class SyncDecisionType {
    None,
    Present,
    DropMissingMetadata,
};

struct SyncDecision {
    SyncDecisionType type{SyncDecisionType::None};
    std::optional<SynchronizedFrame> frame{};
    std::optional<media::FrameKey> droppedKey{};
    std::optional<std::chrono::steady_clock::time_point> droppedDeadline{};
};

class FrameSynchronizer final {
public:
    FrameSynchronizer(
        OverlayFrameBuffer& frames,
        metadata::MetadataStore& metadata)
        : frames_(frames),
          metadata_(metadata) {}

    SyncDecision next(std::chrono::steady_clock::time_point now);

private:
    OverlayFrameBuffer& frames_;
    metadata::MetadataStore& metadata_;
};

} // namespace reg::video
