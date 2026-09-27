#include "metadata/MetadataStore.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace reg::metadata {

MetadataStore::MetadataStore(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("MetadataStore capacity must be greater than zero");
    }
}

std::size_t MetadataStore::FrameKeyHash::operator()(
    const media::FrameKey& key) const noexcept {
    const std::size_t epochHash = std::hash<std::uint64_t>{}(key.streamEpoch);
    const std::size_t frameHash = std::hash<std::uint64_t>{}(key.frameId);

    return epochHash ^
           (frameHash + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
            (epochHash << 6U) + (epochHash >> 2U));
}

MetadataInsertResult MetadataStore::insert(FrameMetadataPtr metadata) {
    if (!metadata) {
        throw std::invalid_argument("MetadataStore::insert received null metadata");
    }

    std::scoped_lock lock(mutex_);

    if (entries_.contains(metadata->key)) {
        return MetadataInsertResult::Duplicate;
    }

    bool evicted = false;
    if (entries_.size() >= capacity_) {
        evictOldestLocked();
        evicted = true;
    }

    const media::FrameKey key = metadata->key;
    insertionOrder_.push_back(key);
    entries_.emplace(key, std::move(metadata));

    return evicted
        ? MetadataInsertResult::EvictedOldestAndInserted
        : MetadataInsertResult::Inserted;
}

FrameMetadataPtr MetadataStore::find(media::FrameKey key) const {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(key);
    return it == entries_.end() ? FrameMetadataPtr{} : it->second;
}

FrameMetadataPtr MetadataStore::take(media::FrameKey key) {
    std::scoped_lock lock(mutex_);

    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return {};
    }

    FrameMetadataPtr result = std::move(it->second);
    entries_.erase(it);

    const auto orderIt = std::find(insertionOrder_.begin(), insertionOrder_.end(), key);
    if (orderIt != insertionOrder_.end()) {
        insertionOrder_.erase(orderIt);
    }

    return result;
}

void MetadataStore::eraseEpoch(std::uint64_t streamEpoch) {
    std::scoped_lock lock(mutex_);

    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.streamEpoch == streamEpoch) {
            it = entries_.erase(it);
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
    entries_.clear();
    insertionOrder_.clear();
}

std::size_t MetadataStore::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

void MetadataStore::evictOldestLocked() {
    if (insertionOrder_.empty()) {
        throw std::logic_error(
            "MetadataStore insertion order lost synchronization with entries");
    }

    const media::FrameKey key = insertionOrder_.front();
    insertionOrder_.pop_front();

    if (entries_.erase(key) == 0) {
        throw std::logic_error(
            "MetadataStore insertion order lost synchronization with entries");
    }
}

} // namespace reg::metadata
