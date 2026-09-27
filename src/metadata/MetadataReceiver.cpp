#include "metadata/MetadataReceiver.hpp"

#include "metadata/Protocol.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace reg::metadata {

MetadataReceiver::MetadataReceiver(
    MetadataStore& store,
    MetadataReceiverConfig config)
    : store_(store),
      config_(std::move(config)),
      socket_(
          config_.bindAddress,
          config_.port,
          config_.receiveTimeout,
          config_.receiveBufferBytes) {}

void MetadataReceiver::run() {
    resetSequenceTracking();

    std::array<std::uint8_t, kReceiveDatagramBufferSize> buffer{};

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const auto received = socket_.receive(buffer);
        if (!received) {
            continue;
        }

        const auto receivedAt = std::chrono::steady_clock::now();
        receivedDatagrams_.fetch_add(1, std::memory_order_relaxed);

        const auto decoded = decodeFrameMetadata(
            std::span<const std::uint8_t>(buffer.data(), *received),
            receivedAt);

        if (!decoded) {
            invalidPackets_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        validPackets_.fetch_add(1, std::memory_order_relaxed);

        const std::uint32_t sequence = decoded->sequence;
        if (isRecentSequence(sequence)) {
            duplicatePackets_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        observeSequence(sequence);
        rememberSequence(sequence);

        const MetadataInsertResult insertResult =
            store_.insert(std::move(*decoded));

        switch (insertResult) {
        case MetadataInsertResult::Inserted:
            break;
        case MetadataInsertResult::Duplicate:
            duplicatePackets_.fetch_add(1, std::memory_order_relaxed);
            break;
        case MetadataInsertResult::Conflict:
            metadataConflicts_.fetch_add(1, std::memory_order_relaxed);
            break;
        case MetadataInsertResult::EvictedOldestAndInserted:
            storeEvictions_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}

void MetadataReceiver::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

MetadataReceiverStats MetadataReceiver::stats() const noexcept {
    return MetadataReceiverStats{
        .receivedDatagrams =
            receivedDatagrams_.load(std::memory_order_relaxed),
        .validPackets =
            validPackets_.load(std::memory_order_relaxed),
        .invalidPackets =
            invalidPackets_.load(std::memory_order_relaxed),
        .duplicatePackets =
            duplicatePackets_.load(std::memory_order_relaxed),
        .outOfOrderPackets =
            outOfOrderPackets_.load(std::memory_order_relaxed),
        .sequenceGapsObserved =
            sequenceGapsObserved_.load(std::memory_order_relaxed),
        .metadataConflicts =
            metadataConflicts_.load(std::memory_order_relaxed),
        .storeEvictions =
            storeEvictions_.load(std::memory_order_relaxed),
    };
}

bool MetadataReceiver::isRecentSequence(std::uint32_t sequence) const noexcept {
    for (std::size_t i = 0; i < recentSequenceCount_; ++i) {
        if (recentSequences_[i] == sequence) {
            return true;
        }
    }
    return false;
}

void MetadataReceiver::rememberSequence(std::uint32_t sequence) noexcept {
    if (recentSequenceCount_ < recentSequences_.size()) {
        recentSequences_[recentSequenceCount_++] = sequence;
        return;
    }

    recentSequences_[recentSequenceCursor_] = sequence;
    recentSequenceCursor_ =
        (recentSequenceCursor_ + 1) % recentSequences_.size();
}

void MetadataReceiver::observeSequence(std::uint32_t sequence) noexcept {
    if (!haveLastSequence_) {
        haveLastSequence_ = true;
        lastSequence_ = sequence;
        return;
    }

    const std::uint32_t delta = sequence - lastSequence_;
    if (delta == 0) {
        return;
    }

    if (delta < 0x80000000U) {
        if (delta > 1) {
            sequenceGapsObserved_.fetch_add(
                static_cast<std::uint64_t>(delta - 1),
                std::memory_order_relaxed);
        }
        lastSequence_ = sequence;
        return;
    }

    outOfOrderPackets_.fetch_add(1, std::memory_order_relaxed);
}

void MetadataReceiver::resetSequenceTracking() noexcept {
    haveLastSequence_ = false;
    lastSequence_ = 0;
    recentSequenceCount_ = 0;
    recentSequenceCursor_ = 0;
    recentSequences_.fill(0);
}

} // namespace reg::metadata
