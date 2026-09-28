#include "metadata/MetadataReceiver.hpp"

#include "metadata/Protocol.hpp"
#include "platform/UdpSocket.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>

namespace reg::metadata {

MetadataReceiver::MetadataReceiver(
    MetadataStore& store,
    MetadataReceiverConfig config)
    : store_(store),
      config_(std::move(config)) {}

void MetadataReceiver::run() {
    stopRequested_.store(false, std::memory_order_release);

    platform::UdpSocket socket(
        config_.bindAddress,
        config_.port,
        config_.receiveTimeoutMs);

    std::array<std::uint8_t, protocol::kMaxDatagramSize> buffer{};
    PacketSequenceTracker sequences;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const std::size_t received = socket.receive(buffer);
        if (received == 0) {
            continue;
        }

        packetsReceived_.fetch_add(1, std::memory_order_relaxed);

        auto decoded = protocol::decodeFrameMetadata(
            std::span<const std::uint8_t>{buffer.data(), received},
            std::chrono::steady_clock::now());

        if (!decoded) {
            packetsInvalid_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const SequenceObservation observation =
            sequences.observe(decoded.metadata.sequence);
        if (observation.duplicate) {
            packetDuplicates_.fetch_add(1, std::memory_order_relaxed);
        }
        if (observation.outOfOrder) {
            packetsOutOfOrder_.fetch_add(1, std::memory_order_relaxed);
        }
        if (observation.missingBefore != 0) {
            sequenceGaps_.fetch_add(
                observation.missingBefore,
                std::memory_order_relaxed);
        }

        const InsertResult insertResult =
            store_.insert(std::move(decoded.metadata));

        if (insertResult == InsertResult::Duplicate) {
            frameDuplicates_.fetch_add(1, std::memory_order_relaxed);
        } else if (insertResult == InsertResult::EvictedOldest) {
            storeEvictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MetadataReceiver::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

MetadataReceiverStats MetadataReceiver::stats() const noexcept {
    return MetadataReceiverStats{
        .packetsReceived = packetsReceived_.load(std::memory_order_relaxed),
        .packetsInvalid = packetsInvalid_.load(std::memory_order_relaxed),
        .packetDuplicates = packetDuplicates_.load(std::memory_order_relaxed),
        .packetsOutOfOrder = packetsOutOfOrder_.load(std::memory_order_relaxed),
        .sequenceGaps = sequenceGaps_.load(std::memory_order_relaxed),
        .frameDuplicates = frameDuplicates_.load(std::memory_order_relaxed),
        .storeEvictions = storeEvictions_.load(std::memory_order_relaxed),
    };
}

} // namespace reg::metadata
