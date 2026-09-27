#pragma once

#include "metadata/FrameMetadata.hpp"
#include "metadata/PacketSequenceTracker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace reg::metadata {

struct UdpMetadataReceiverConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{};
    std::chrono::milliseconds receiveTimeout{100};
};

struct UdpMetadataReceiverStats {
    std::uint64_t datagramsReceived{};
    std::uint64_t packetsAccepted{};
    std::uint64_t invalidPackets{};
    std::uint64_t sequenceDuplicates{};
    std::uint64_t sequenceOutOfOrder{};
    std::uint64_t sequenceStale{};
    std::uint64_t sequenceGaps{};
    std::uint64_t estimatedMissingPackets{};
};

class UdpMetadataReceiver final {
public:
    using Callback = std::function<void(FrameMetadataPtr)>;

    explicit UdpMetadataReceiver(UdpMetadataReceiverConfig config);
    ~UdpMetadataReceiver();

    UdpMetadataReceiver(const UdpMetadataReceiver&) = delete;
    UdpMetadataReceiver& operator=(const UdpMetadataReceiver&) = delete;

    void run(const Callback& onMetadata);
    void requestStop() noexcept;

    std::uint16_t boundPort() const noexcept { return boundPort_; }
    UdpMetadataReceiverStats stats() const noexcept;

private:
#ifdef _WIN32
    using NativeSocket = std::uintptr_t;
    static constexpr NativeSocket kInvalidSocket =
        static_cast<NativeSocket>(~static_cast<std::uintptr_t>(0));
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    void initializeSocket();
    void closeSocket() noexcept;
    void handleAcceptedSequence(std::uint32_t sequence) noexcept;

    UdpMetadataReceiverConfig config_;
    NativeSocket socket_{kInvalidSocket};
    std::uint16_t boundPort_{};

    PacketSequenceTracker sequenceTracker_;

    std::atomic_bool stopRequested_{false};

    std::atomic_uint64_t datagramsReceived_{0};
    std::atomic_uint64_t packetsAccepted_{0};
    std::atomic_uint64_t invalidPackets_{0};
    std::atomic_uint64_t sequenceDuplicates_{0};
    std::atomic_uint64_t sequenceOutOfOrder_{0};
    std::atomic_uint64_t sequenceStale_{0};
    std::atomic_uint64_t sequenceGaps_{0};
    std::atomic_uint64_t estimatedMissingPackets_{0};

#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::metadata
