#include "video/FrameSynchronizer.hpp"

#include <utility>

namespace reg::video {

SyncDecision FrameSynchronizer::next(
    std::chrono::steady_clock::time_point now) {
    auto due = frames_.takeDue(now);
    if (!due) {
        return {};
    }

    auto metadata = metadata_.take(due->key);
    if (!metadata) {
        return SyncDecision{
            .type = SyncDecisionType::DropMissingMetadata,
            .frame = std::nullopt,
            .droppedKey = due->key,
            .droppedDeadline = due->deadline,
        };
    }

    return SyncDecision{
        .type = SyncDecisionType::Present,
        .frame = SynchronizedFrame{
            .buffered = std::move(*due),
            .metadata = std::move(metadata),
        },
        .droppedKey = std::nullopt,
        .droppedDeadline = std::nullopt,
    };
}

} // namespace reg::video
