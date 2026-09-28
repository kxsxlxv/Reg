#pragma once

#include "metadata/FrameMetadata.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace reg::metadata {

enum class InsertResult {
    Inserted,
    Duplicate,
    EvictedOldest,
};

class MetadataStore final {
public:
    explicit MetadataStore(std::size_t capacity = 512);

    InsertResult insert(FrameMetadata metadata);

    FrameMetadataPtr find(media::FrameKey key) const;
    FrameMetadataPtr take(media::FrameKey key);

    void eraseEpoch(std::uint64_t streamEpoch);
    void clear();

    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    using Map = std::unordered_map<
        media::FrameKey,
        FrameMetadataPtr,
        media::FrameKeyHash>;

    void eraseFromInsertionOrderLocked(media::FrameKey key);

    std::size_t capacity_{};
    mutable std::mutex mutex_;
    Map metadata_;
    std::deque<media::FrameKey> insertionOrder_;
};

} // namespace reg::metadata
