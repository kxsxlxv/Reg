#pragma once

#include "metadata/MetadataStore.hpp"
#include "platform/UdpSocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace reg::metadata {

struct MetadataReceiverConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{5000};
    std::chrono::milliseconds pollTimeout{20};
};

struct MetadataReceiverStats {
    std::uint64_t datagramsReceived{};
    std::uint64_t packetsAccepted{};
    std::uint64_t invalidPackets{};
    std::uint64_t duplicateSequences{};
    std::uint64_t duplicateFrames{};
    std::uint64_t outOfOrderSequences{};
    std::uint64_t sequenceGapsObserved{};
    std::uint64_t latePackets{};
    std::uint64_t storeEvictions{};
};

class MetadataReceiver final {
public:
    MetadataReceiver(
        MetadataStore& store,
        MetadataReceiverConfig config);

    MetadataReceiver(const MetadataReceiver&) = delete;
    MetadataReceiver& operator=(const MetadataReceiver&) = delete;

    void run();
    void requestStop() noexcept;

    MetadataReceiverStats stats() const noexcept;

private:
    enum class SequenceObservation {
        First,
        InOrder,
        Gap,
        OutOfOrder,
        Duplicate,
    };

    SequenceObservation observeSequence(std::uint32_t sequence) noexcept;

    MetadataStore& store_;
    MetadataReceiverConfig config_;
    platform::UdpSocket socket_;

    std::atomic_bool stopRequested_{false};

    bool haveHighestSequence_{false};
    std::uint32_t highestSequence_{0};

    std::atomic_uint64_t datagramsReceived_{0};
    std::atomic_uint64_t packetsAccepted_{0};
    std::atomic_uint64_t invalidPackets_{0};
    std::atomic_uint64_t duplicateSequences_{0};
    std::atomic_uint64_t duplicateFrames_{0};
    std::atomic_uint64_t outOfOrderSequences_{0};
    std::atomic_uint64_t sequenceGapsObserved_{0};
    std::atomic_uint64_t latePackets_{0};
    std::atomic_uint64_t storeEvictions_{0};
};

} // namespace reg::metadata
