#pragma once

#include "metadata/FrameMetadata.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace reg::metadata {

enum class MetadataInsertResult {
    Inserted,
    Duplicate,
    TooLate,
    EvictedOldestAndInserted,
};

class MetadataStore final {
public:
    explicit MetadataStore(std::size_t capacity = 512);

    MetadataInsertResult insert(FrameMetadataPtr metadata);

    FrameMetadataPtr find(media::FrameKey key) const;
    FrameMetadataPtr take(media::FrameKey key);

    // Advances the display watermark. Metadata for this FrameKey or anything
    // older is no longer useful for live presentation and will be rejected.
    void discardThrough(media::FrameKey key);

    void eraseEpoch(std::uint64_t streamEpoch);
    void clear();

    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct FrameKeyHash {
        std::size_t operator()(const media::FrameKey& key) const noexcept;
    };

    void evictOldestLocked();

    const std::size_t capacity_{};
    mutable std::mutex mutex_;
    std::unordered_map<media::FrameKey, FrameMetadataPtr, FrameKeyHash> entries_;
    std::deque<media::FrameKey> insertionOrder_;
    std::optional<media::FrameKey> discardedThrough_;
};

} // namespace reg::metadata
