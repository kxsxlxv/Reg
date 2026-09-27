#pragma once

#include "metadata/FrameMetadata.hpp"

#include <cstddef>
#include <deque>
#include <unordered_map>

namespace reg::metadata {

enum class MetadataInsertResult {
    Inserted,
    Duplicate,
    EvictedOldestAndInserted,
};

class MetadataStore final {
public:
    explicit MetadataStore(std::size_t capacity = 512);

    MetadataInsertResult insert(FrameMetadataPtr metadata);

    FrameMetadataPtr find(media::FrameKey key) const;
    FrameMetadataPtr take(media::FrameKey key);

    void eraseEpoch(std::uint64_t streamEpoch);
    void clear() noexcept;

    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct FrameKeyHash {
        std::size_t operator()(const media::FrameKey& key) const noexcept;
    };

    void evictOldest();

    std::size_t capacity_{};
    std::unordered_map<media::FrameKey, FrameMetadataPtr, FrameKeyHash> entries_;
    std::deque<media::FrameKey> insertionOrder_;
};

} // namespace reg::metadata
