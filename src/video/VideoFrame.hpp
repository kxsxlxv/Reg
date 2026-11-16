#pragma once

#include "media/FrameIdentity.hpp"
#include "media/GtSei.hpp"
#include "media/GtAccessUnit.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <chrono>
#include <memory>
#include <optional>

namespace reg::video {

class VideoFrame final {
public:
    static std::shared_ptr<const VideoFrame> cloneFrom(
        const AVFrame* source,
        std::shared_ptr<media::FrameGtParts> fromPacket = {});

    ~VideoFrame();

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) = delete;
    VideoFrame& operator=(VideoFrame&&) = delete;

    AVFrame* avFrame() const noexcept { return frame_; }
    AVVkFrame* vkFrame() const noexcept;

    int width() const noexcept { return frame_->width; }
    int height() const noexcept { return frame_->height; }
    std::optional<media::SourceFrameIdentity> identity() const noexcept { return parts_->source; }
    const media::GtMetadata& gt() const noexcept { return parts_->bbox; }
    const media::CameraImuMetadata& cameraImu() const noexcept {
        return parts_->bbox.cameraImu;
    }
    const media::GtMapMetadata& skyGt() const noexcept { return parts_->sky; }
    const media::GtMapMetadata& depthGt() const noexcept { return parts_->depth; }
    const media::FrameGtParts& allGt() const noexcept { return *parts_; }
    std::chrono::steady_clock::time_point decodedAt() const noexcept { return decodedAt_; }

private:
    VideoFrame(AVFrame* frame, std::shared_ptr<media::FrameGtParts> fromPacket);

    AVFrame* frame_{nullptr};
    std::shared_ptr<const media::FrameGtParts> parts_;
    std::chrono::steady_clock::time_point decodedAt_{};
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

} // namespace reg::video
