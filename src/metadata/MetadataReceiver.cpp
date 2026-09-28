#include "metadata/MetadataReceiver.hpp"

#include "metadata/MetadataProtocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace reg::metadata {

MetadataReceiver::MetadataReceiver(
    MetadataStore& store,
    MetadataReceiverConfig config)
    : store_(store),
      config_(std::move(config)) {
    if (config_.bindAddress.empty()) {
        throw std::invalid_argument("Metadata bind address must not be empty");
    }
    if (config_.port == 0) {
        throw std::invalid_argument("Metadata UDP port must be non-zero");
    }
    if (config_.pollTimeout.count() <= 0) {
        throw std::invalid_argument("Metadata poll timeout must be positive");
    }

    socket_.bindIpv4(config_.bindAddress, config_.port);
}

void MetadataReceiver::run() {
    stopRequested_.store(false, std::memory_order_release);

    // Maximum IPv4 UDP payload. The protocol parser itself rejects anything
    // above the much smaller application-level datagram limit.
    std::array<std::uint8_t, 65'507> buffer{};

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const auto received = socket_.receive(buffer, config_.pollTimeout);
        if (!received) {
            continue;
        }

        datagramsReceived_.fetch_add(1, std::memory_order_relaxed);

        const auto decoded = wire::decodeFrameMetadataPacket(
            std::span<const std::uint8_t>(buffer.data(), *received),
            std::chrono::steady_clock::now());

        if (!decoded) {
            invalidPackets_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const auto sequenceObservation =
            observeSequence(decoded.metadata->packetSequence);

        if (sequenceObservation == SequenceObservation::Duplicate) {
            duplicateSequences_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (sequenceObservation == SequenceObservation::OutOfOrder) {
            outOfOrderSequences_.fetch_add(1, std::memory_order_relaxed);
        }

        switch (store_.insert(decoded.metadata)) {
        case MetadataInsertResult::Inserted:
            packetsAccepted_.fetch_add(1, std::memory_order_relaxed);
            break;

        case MetadataInsertResult::Duplicate:
            duplicateFrames_.fetch_add(1, std::memory_order_relaxed);
            break;

        case MetadataInsertResult::TooLate:
            latePackets_.fetch_add(1, std::memory_order_relaxed);
            break;

        case MetadataInsertResult::EvictedOldestAndInserted:
            packetsAccepted_.fetch_add(1, std::memory_order_relaxed);
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
        .datagramsReceived = datagramsReceived_.load(std::memory_order_relaxed),
        .packetsAccepted = packetsAccepted_.load(std::memory_order_relaxed),
        .invalidPackets = invalidPackets_.load(std::memory_order_relaxed),
        .duplicateSequences = duplicateSequences_.load(std::memory_order_relaxed),
        .duplicateFrames = duplicateFrames_.load(std::memory_order_relaxed),
        .outOfOrderSequences = outOfOrderSequences_.load(std::memory_order_relaxed),
        .sequenceGapsObserved = sequenceGapsObserved_.load(std::memory_order_relaxed),
        .latePackets = latePackets_.load(std::memory_order_relaxed),
        .storeEvictions = storeEvictions_.load(std::memory_order_relaxed),
    };
}

MetadataReceiver::SequenceObservation MetadataReceiver::observeSequence(
    std::uint32_t sequence) noexcept {
    if (!haveHighestSequence_) {
        haveHighestSequence_ = true;
        highestSequence_ = sequence;
        return SequenceObservation::First;
    }

    // Unsigned subtraction followed by signed interpretation gives a
    // wrap-aware ordering as long as no discontinuity spans >= 2^31 packets.
    const auto delta =
        static_cast<std::int32_t>(sequence - highestSequence_);

    if (delta == 0) {
        return SequenceObservation::Duplicate;
    }

    if (delta < 0) {
        return SequenceObservation::OutOfOrder;
    }

    highestSequence_ = sequence;

    if (delta == 1) {
        return SequenceObservation::InOrder;
    }

    sequenceGapsObserved_.fetch_add(
        static_cast<std::uint64_t>(delta - 1),
        std::memory_order_relaxed);
    return SequenceObservation::Gap;
}

} // namespace reg::metadata
