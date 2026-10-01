#include "metadata/MetadataStore.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace reg::metadata {
namespace {

bool sequenceIsNewer(
    const std::uint32_t candidate,
    const std::uint32_t current) noexcept {
    const std::uint32_t delta = candidate - current;
    return delta != 0U && delta < 0x80000000U;
}

} // namespace

MetadataStore::MetadataStore(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("MetadataStore capacity must be greater than zero");
    }
}

InsertResult MetadataStore::insert(FrameMetadata metadata) {
    auto value = std::make_shared<const FrameMetadata>(std::move(metadata));
    const media::FrameKey key = value->key;

    std::scoped_lock lock(mutex_);

    if (const auto existing = metadata_.find(key);
        existing != metadata_.end()) {
        if (!sequenceIsNewer(
                value->sequence,
                existing->second->sequence)) {
            return InsertResult::Duplicate;
        }

        // Same exact FrameKey, newer CVM1 snapshot. Keep its position in the
        // bounded insertion-order queue and upgrade only the value. This is not
        // approximate synchronization: both packets explicitly name the same
        // canonical source frame.
        existing->second = std::move(value);
        return InsertResult::Replaced;
    }

    InsertResult result = InsertResult::Inserted;
    if (metadata_.size() >= capacity_) {
        while (!insertionOrder_.empty()) {
            const media::FrameKey oldest = insertionOrder_.front();
            insertionOrder_.pop_front();
            if (metadata_.erase(oldest) != 0) {
                result = InsertResult::EvictedOldest;
                break;
            }
        }
    }

    metadata_.emplace(key, std::move(value));
    insertionOrder_.push_back(key);
    return result;
}

FrameMetadataPtr MetadataStore::find(media::FrameKey key) const {
    std::scoped_lock lock(mutex_);
    const auto it = metadata_.find(key);
    return it == metadata_.end() ? FrameMetadataPtr{} : it->second;
}

FrameMetadataPtr MetadataStore::take(media::FrameKey key) {
    std::scoped_lock lock(mutex_);
    const auto it = metadata_.find(key);
    if (it == metadata_.end()) {
        return {};
    }

    FrameMetadataPtr value = std::move(it->second);
    metadata_.erase(it);
    eraseFromInsertionOrderLocked(key);
    return value;
}

void MetadataStore::eraseEpoch(std::uint64_t streamEpoch) {
    std::scoped_lock lock(mutex_);

    for (auto it = metadata_.begin(); it != metadata_.end();) {
        if (it->first.streamEpoch == streamEpoch) {
            it = metadata_.erase(it);
        } else {
            ++it;
        }
    }

    std::erase_if(insertionOrder_, [streamEpoch](const media::FrameKey& key) {
        return key.streamEpoch == streamEpoch;
    });
}

void MetadataStore::clear() {
    std::scoped_lock lock(mutex_);
    metadata_.clear();
    insertionOrder_.clear();
}

std::size_t MetadataStore::size() const {
    std::scoped_lock lock(mutex_);
    return metadata_.size();
}

void MetadataStore::eraseFromInsertionOrderLocked(media::FrameKey key) {
    const auto it = std::ranges::find(insertionOrder_, key);
    if (it != insertionOrder_.end()) {
        insertionOrder_.erase(it);
    }
}

} // namespace reg::metadata
