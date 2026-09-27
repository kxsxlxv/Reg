#pragma once

#include "metadata/MetadataStore.hpp"
#include "platform/UdpSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace reg::metadata {

struct MetadataReceiverConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{};
    std::chrono::milliseconds receiveTimeout{100};
    int receiveBufferBytes{1 << 20};
};

struct MetadataReceiverStats {
    std::uint64_t receivedDatagrams{};
    std::uint64_t validPackets{};
    std::uint64_t invalidPackets{};
    std::uint64_t duplicatePackets{};
    std::uint64_t outOfOrderPackets{};
    std::uint64_t sequenceGapsObserved{};
    std::uint64_t metadataConflicts{};
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

    std::uint16_t localPort() const noexcept { return socket_.localPort(); }
    MetadataReceiverStats stats() const noexcept;

private:
    bool isRecentSequence(std::uint32_t sequence) const noexcept;
    void rememberSequence(std::uint32_t sequence) noexcept;
    void observeSequence(std::uint32_t sequence) noexcept;
    void resetSequenceTracking() noexcept;

    static constexpr std::size_t kRecentSequenceWindow = 256;
    static constexpr std::size_t kReceiveDatagramBufferSize = 4096;

    MetadataStore& store_;
    MetadataReceiverConfig config_;
    platform::UdpSocket socket_;

    std::atomic_bool stopRequested_{false};

    bool haveLastSequence_{false};
    std::uint32_t lastSequence_{};
    std::array<std::uint32_t, kRecentSequenceWindow> recentSequences_{};
    std::size_t recentSequenceCount_{0};
    std::size_t recentSequenceCursor_{0};

    std::atomic_uint64_t receivedDatagrams_{0};
    std::atomic_uint64_t validPackets_{0};
    std::atomic_uint64_t invalidPackets_{0};
    std::atomic_uint64_t duplicatePackets_{0};
    std::atomic_uint64_t outOfOrderPackets_{0};
    std::atomic_uint64_t sequenceGapsObserved_{0};
    std::atomic_uint64_t metadataConflicts_{0};
    std::atomic_uint64_t storeEvictions_{0};
};

} // namespace reg::metadata
