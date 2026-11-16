#include "video/VideoFrame.hpp"

#include <stdexcept>

namespace reg::video {

VideoFrame::VideoFrame(AVFrame* frame,
                       std::shared_ptr<media::FrameGtParts> fromPacket)
    : frame_(frame), decodedAt_(std::chrono::steady_clock::now()) {
    if (!fromPacket) {
        // Legacy fallback only when no packet PTS association was available.
        fromPacket = std::make_shared<media::FrameGtParts>();
        fromPacket->source = media::extractFrameIdentity(frame);
        fromPacket->bbox = media::extractGtSei(frame);
    }
    media::validateGtForFrame(fromPacket->bbox, fromPacket->source,
                              frame->width, frame->height);
    media::validateGtMapForFrame(fromPacket->sky, fromPacket->source);
    media::validateGtMapForFrame(fromPacket->depth, fromPacket->source);
    parts_ = std::move(fromPacket); // immutable after frame creation
}

VideoFrame::~VideoFrame() {
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
    }
}

std::shared_ptr<const VideoFrame> VideoFrame::cloneFrom(
    const AVFrame* source, std::shared_ptr<media::FrameGtParts> fromPacket) {
    if (source == nullptr) {
        throw std::invalid_argument("VideoFrame::cloneFrom received a null AVFrame");
    }
    if (source->format != AV_PIX_FMT_VULKAN) {
        throw std::runtime_error("Decoded-frame CPU/software fallback is forbidden: expected AV_PIX_FMT_VULKAN");
    }

    AVFrame* clone = av_frame_clone(source);
    if (clone == nullptr) {
        throw std::bad_alloc{};
    }

    return std::shared_ptr<const VideoFrame>(new VideoFrame(clone, std::move(fromPacket)));
}

AVVkFrame* VideoFrame::vkFrame() const noexcept {
    if (frame_ == nullptr || frame_->format != AV_PIX_FMT_VULKAN) {
        return nullptr;
    }
    return reinterpret_cast<AVVkFrame*>(frame_->data[0]);
}

} // namespace reg::video
