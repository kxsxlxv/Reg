#pragma once

#include "metadata/Protocol.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace reg::metadata {

enum class MetadataInsertResult {
    Inserted,
    Duplicate,
    Conflict,
    EvictedOldestAndInserted,
};

class MetadataStore final {
public:
    using MetadataPtr = std::shared_ptr<const FrameMetadata>;
    using Clock = std::chrono::steady_clock;

    explicit MetadataStore(
        std::size_t capacity = 512,
        std::chrono::milliseconds maxAge = std::chrono::seconds(2));

    MetadataInsertResult insert(FrameMetadata metadata);

    MetadataPtr find(media::FrameKey key) const;
    MetadataPtr take(media::FrameKey key);

    std::size_t purgeExpired(Clock::time_point now);
    void clear();

    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    using Map = std::unordered_map<
        media::FrameKey,
        MetadataPtr,
        media::FrameKeyHash>;

    Map::iterator oldestEntryUnlocked();
    std::size_t purgeExpiredUnlocked(Clock::time_point now);

    std::size_t capacity_{};
    std::chrono::milliseconds maxAge_{};
    mutable std::mutex mutex_;
    Map entries_;
};

} // namespace reg::metadata
