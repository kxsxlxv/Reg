#include "metadata/MetadataSender.hpp"

#include "metadata/Protocol.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace reg::metadata {

MetadataSender::MetadataSender(
    std::string_view remoteAddress,
    std::uint16_t remotePort,
    std::uint32_t initialSequence)
    : socket_(
          remoteAddress,
          remotePort),
      nextSequence_(
          initialSequence) {}

bool MetadataSender::send(
    FrameMetadata metadata) {
    metadata.sequence =
        nextSequence_++;

    ++attempted_;

    std::array<
        std::uint8_t,
        protocol::kMaxDatagramSize>
        packet{};

    const std::size_t packetSize =
        protocol::encodeFrameMetadataInto(
            metadata,
            packet);

    const bool sent =
        socket_.trySend(
            std::span<
                const std::uint8_t>{
                packet.data(),
                packetSize});

    if (sent) {
        ++sent_;
    } else {
        ++droppedWouldBlock_;
    }

    return sent;
}

MetadataSenderStats
MetadataSender::stats() const noexcept {
    return MetadataSenderStats{
        .attempted = attempted_,
        .sent = sent_,
        .droppedWouldBlock =
            droppedWouldBlock_,
        .nextSequence = nextSequence_,
    };
}

} // namespace reg::metadata
