#pragma once

#include "metadata/MetadataStore.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace reg::video {

enum class SyncAction {
    Wait,
    Present,
    DropMissingMetadata,
};

struct SynchronizedFrame {
    VideoFramePtr video;
    metadata::MetadataStore::MetadataPtr metadata;
};

struct SyncResult {
    SyncAction action{SyncAction::Wait};
    media::FrameKey key{};
    SynchronizedFrame frame{};
};

struct SynchronizerStats {
    std::uint64_t presented{};
    std::uint64_t droppedMissingMetadata{};
};

class FrameSynchronizer final {
public:
    using Clock = std::chrono::steady_clock;

    FrameSynchronizer(
        OverlayFrameBuffer& videoFrames,
        metadata::MetadataStore& metadataStore)
        : videoFrames_(videoFrames),
          metadataStore_(metadataStore) {}

    SyncResult next(Clock::time_point now);

    void clear();
    SynchronizerStats stats() const noexcept;

private:
    OverlayFrameBuffer& videoFrames_;
    metadata::MetadataStore& metadataStore_;

    std::atomic_uint64_t presented_{0};
    std::atomic_uint64_t droppedMissingMetadata_{0};
};

} // namespace reg::video
