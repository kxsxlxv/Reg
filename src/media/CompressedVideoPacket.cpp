#include "media/CompressedVideoPacket.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <new>
#include <stdexcept>

namespace reg::media {

CompressedVideoPacket::CompressedVideoPacket(
    AVPacket* packet,
    AVRational timeBase,
    std::chrono::steady_clock::time_point receivedAt) noexcept
    : packet_(packet),
      timeBase_(timeBase),
      receivedAt_(receivedAt) {}

CompressedVideoPacket::~CompressedVideoPacket() {
    av_packet_free(&packet_);
}

std::shared_ptr<const CompressedVideoPacket>
CompressedVideoPacket::cloneFrom(
    const AVPacket* packet,
    AVRational timeBase) {
    if (packet == nullptr) {
        throw std::invalid_argument(
            "CompressedVideoPacket requires AVPacket");
    }

    AVPacket* clone = av_packet_clone(packet);
    if (clone == nullptr) {
        throw std::bad_alloc{};
    }

    return std::shared_ptr<
        const CompressedVideoPacket>(
            new CompressedVideoPacket(
                clone,
                timeBase,
                std::chrono::steady_clock::now()));
}

bool CompressedVideoPacket::keyFrame() const noexcept {
    return packet_ != nullptr &&
           (packet_->flags & AV_PKT_FLAG_KEY) != 0;
}

} // namespace reg::media
