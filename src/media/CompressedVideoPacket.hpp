#pragma once

#include <memory>

extern "C" {
#include <libavutil/rational.h>
}

struct AVPacket;

namespace reg::media {

class CompressedVideoPacket final {
public:
    static std::shared_ptr<const CompressedVideoPacket>
    cloneFrom(
        const AVPacket* packet,
        AVRational timeBase);

    ~CompressedVideoPacket();

    CompressedVideoPacket(
        const CompressedVideoPacket&) = delete;
    CompressedVideoPacket& operator=(
        const CompressedVideoPacket&) = delete;

    AVPacket* avPacket() const noexcept {
        return packet_;
    }

    AVRational timeBase() const noexcept {
        return timeBase_;
    }

    bool keyFrame() const noexcept;

private:
    CompressedVideoPacket(
        AVPacket* packet,
        AVRational timeBase) noexcept;

    AVPacket* packet_{nullptr};
    AVRational timeBase_{};
};

using CompressedVideoPacketPtr =
    std::shared_ptr<const CompressedVideoPacket>;

} // namespace reg::media
