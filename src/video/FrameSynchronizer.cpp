#include "video/FrameSynchronizer.hpp"

#include <utility>

namespace reg::video {

SyncResult FrameSynchronizer::next(Clock::time_point now) {
    // Keep metadata bounded even when video stalls or a sender continues after
    // the corresponding frame has already been discarded.
    metadataStore_.purgeExpired(now);

    auto due = videoFrames_.popDue(now);
    if (!due) {
        return SyncResult{.action = SyncAction::Wait};
    }

    const media::FrameKey key = due->key;
    auto metadata = metadataStore_.take(key);

    if (!metadata) {
        droppedMissingMetadata_.fetch_add(1, std::memory_order_relaxed);
        return SyncResult{
            .action = SyncAction::DropMissingMetadata,
            .key = key,
        };
    }

    presented_.fetch_add(1, std::memory_order_relaxed);
    return SyncResult{
        .action = SyncAction::Present,
        .key = key,
        .frame = SynchronizedFrame{
            .video = std::move(due->video),
            .metadata = std::move(metadata),
        },
    };
}

void FrameSynchronizer::clear() {
    videoFrames_.clear();
    metadataStore_.clear();
}

SynchronizerStats FrameSynchronizer::stats() const noexcept {
    return SynchronizerStats{
        .presented = presented_.load(std::memory_order_relaxed),
        .droppedMissingMetadata =
            droppedMissingMetadata_.load(std::memory_order_relaxed),
    };
}

} // namespace reg::video
