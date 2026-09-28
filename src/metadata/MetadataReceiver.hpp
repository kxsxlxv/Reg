#pragma once

#include "metadata/MetadataStore.hpp"
#include "metadata/PacketSequenceTracker.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace reg::metadata {

struct MetadataReceiverConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{50010};
    std::uint32_t receiveTimeoutMs{100};
};

struct MetadataReceiverStats {
    std::uint64_t packetsReceived{};
    std::uint64_t packetsInvalid{};
    std::uint64_t packetDuplicates{};
    std::uint64_t packetsOutOfOrder{};
    std::uint64_t sequenceGaps{};
    std::uint64_t frameDuplicates{};
    std::uint64_t storeEvictions{};
};

class MetadataReceiver final {
public:
    MetadataReceiver(
        MetadataStore& store,
        MetadataReceiverConfig config = {});

    void run();
    void requestStop() noexcept;

    MetadataReceiverStats stats() const noexcept;

private:
    MetadataStore& store_;
    MetadataReceiverConfig config_;
    std::atomic_bool stopRequested_{false};

    std::atomic_uint64_t packetsReceived_{0};
    std::atomic_uint64_t packetsInvalid_{0};
    std::atomic_uint64_t packetDuplicates_{0};
    std::atomic_uint64_t packetsOutOfOrder_{0};
    std::atomic_uint64_t sequenceGaps_{0};
    std::atomic_uint64_t frameDuplicates_{0};
    std::atomic_uint64_t storeEvictions_{0};
};

} // namespace reg::metadata
