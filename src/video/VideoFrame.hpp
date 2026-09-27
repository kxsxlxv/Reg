#pragma once

#include "media/FrameIdentity.hpp"

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
    static std::shared_ptr<const VideoFrame> cloneFrom(const AVFrame* source);

    ~VideoFrame();

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) = delete;
    VideoFrame& operator=(VideoFrame&&) = delete;

    AVFrame* avFrame() const noexcept { return frame_; }
    AVVkFrame* vkFrame() const noexcept;

    int width() const noexcept { return frame_->width; }
    int height() const noexcept { return frame_->height; }
    std::optional<media::SourceFrameIdentity> identity() const noexcept { return identity_; }
    std::chrono::steady_clock::time_point decodedAt() const noexcept { return decodedAt_; }

private:
    explicit VideoFrame(AVFrame* frame);

    AVFrame* frame_{nullptr};
    std::optional<media::SourceFrameIdentity> identity_;
    std::chrono::steady_clock::time_point decodedAt_{};
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

} // namespace reg::video
