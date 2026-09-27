#include "metadata/MetadataStore.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace reg::metadata {

MetadataStore::MetadataStore(
    std::size_t capacity,
    std::chrono::milliseconds maxAge)
    : capacity_(capacity),
      maxAge_(maxAge) {
    if (capacity_ == 0) {
        throw std::invalid_argument("MetadataStore capacity must be greater than zero");
    }
    if (maxAge_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("MetadataStore maxAge must be positive");
    }

    entries_.reserve(capacity_);
}

MetadataInsertResult MetadataStore::insert(FrameMetadata metadata) {
    if (metadata.receivedAt == Clock::time_point{}) {
        metadata.receivedAt = Clock::now();
    }

    std::scoped_lock lock(mutex_);
    purgeExpiredUnlocked(metadata.receivedAt);

    const auto existing = entries_.find(metadata.key);
    if (existing != entries_.end()) {
        return existing->second->sequence == metadata.sequence
            ? MetadataInsertResult::Duplicate
            : MetadataInsertResult::Conflict;
    }

    bool evicted = false;
    if (entries_.size() >= capacity_) {
        const auto oldest = oldestEntryUnlocked();
        if (oldest != entries_.end()) {
            entries_.erase(oldest);
            evicted = true;
        }
    }

    auto stored = std::make_shared<const FrameMetadata>(std::move(metadata));
    entries_.emplace(stored->key, std::move(stored));

    return evicted
        ? MetadataInsertResult::EvictedOldestAndInserted
        : MetadataInsertResult::Inserted;
}

MetadataStore::MetadataPtr MetadataStore::find(media::FrameKey key) const {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : found->second;
}

MetadataStore::MetadataPtr MetadataStore::take(media::FrameKey key) {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return nullptr;
    }

    MetadataPtr result = std::move(found->second);
    entries_.erase(found);
    return result;
}

std::size_t MetadataStore::purgeExpired(Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    return purgeExpiredUnlocked(now);
}

std::size_t MetadataStore::purgeExpiredUnlocked(Clock::time_point now) {
    std::size_t removed = 0;

    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto receivedAt = it->second->receivedAt;
        if (receivedAt != Clock::time_point{} &&
            now >= receivedAt &&
            now - receivedAt > maxAge_) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    return removed;
}

void MetadataStore::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

std::size_t MetadataStore::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

MetadataStore::Map::iterator MetadataStore::oldestEntryUnlocked() {
    return std::min_element(
        entries_.begin(),
        entries_.end(),
        [](const auto& left, const auto& right) {
            return left.second->receivedAt < right.second->receivedAt;
        });
}

} // namespace reg::metadata
