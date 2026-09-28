#pragma once

#include "metadata/FrameMetadata.hpp"
#include "platform/UdpSender.hpp"

#include <cstdint>
#include <string_view>

namespace reg::metadata {

struct MetadataSenderStats {
    std::uint64_t attempted{};
    std::uint64_t sent{};
    std::uint64_t droppedWouldBlock{};
    std::uint32_t nextSequence{};
};

class MetadataSender final {
public:
    MetadataSender(
        std::string_view remoteAddress,
        std::uint16_t remotePort,
        std::uint32_t initialSequence = 0);

    // Assigns the transport sequence number and sends one CVM1 datagram.
    // Returns false if the non-blocking UDP socket would block.
    bool send(FrameMetadata metadata);

    MetadataSenderStats stats() const noexcept;

private:
    platform::UdpSender socket_;
    std::uint32_t nextSequence_{};

    std::uint64_t attempted_{};
    std::uint64_t sent_{};
    std::uint64_t droppedWouldBlock_{};
};

} // namespace reg::metadata
