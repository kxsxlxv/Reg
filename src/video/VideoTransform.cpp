#include "video/VideoTransform.hpp"

#include <algorithm>
#include <stdexcept>

namespace reg::video {

VideoTransform::VideoTransform(
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
    : sourceWidth_(sourceWidth),
      sourceHeight_(sourceHeight),
      targetWidth_(targetWidth),
      targetHeight_(targetHeight) {
    if (sourceWidth_ == 0 || sourceHeight_ == 0) {
        throw std::invalid_argument("VideoTransform source dimensions must be non-zero");
    }
    if (targetWidth_ == 0 || targetHeight_ == 0) {
        throw std::invalid_argument("VideoTransform target dimensions must be non-zero");
    }

    recomputeViewport();
}

void VideoTransform::setZoom(float zoom) {
    if (!(zoom >= 1.0F)) {
        throw std::invalid_argument("VideoTransform zoom must be >= 1");
    }
    zoom_ = zoom;
}

void VideoTransform::recomputeViewport() {
    const float sourceAspect =
        static_cast<float>(sourceWidth_) /
        static_cast<float>(sourceHeight_);
    const float targetAspect =
        static_cast<float>(targetWidth_) /
        static_cast<float>(targetHeight_);

    if (targetAspect > sourceAspect) {
        viewport_.height = static_cast<float>(targetHeight_);
        viewport_.width = viewport_.height * sourceAspect;
        viewport_.x =
            (static_cast<float>(targetWidth_) - viewport_.width) * 0.5F;
        viewport_.y = 0.0F;
    } else {
        viewport_.width = static_cast<float>(targetWidth_);
        viewport_.height = viewport_.width / sourceAspect;
        viewport_.x = 0.0F;
        viewport_.y =
            (static_cast<float>(targetHeight_) - viewport_.height) * 0.5F;
    }
}

Vec2 VideoTransform::normalizedToScreen(Vec2 normalized) const noexcept {
    const Vec2 center{
        .x = 0.5F + pan_.x,
        .y = 0.5F + pan_.y,
    };

    const Vec2 zoomed{
        .x = (normalized.x - center.x) * zoom_ + 0.5F,
        .y = (normalized.y - center.y) * zoom_ + 0.5F,
    };

    return Vec2{
        .x = viewport_.x + zoomed.x * viewport_.width,
        .y = viewport_.y + zoomed.y * viewport_.height,
    };
}

RectF VideoTransform::normalizedToScreen(RectF normalized) const noexcept {
    const Vec2 topLeft =
        normalizedToScreen(Vec2{normalized.x, normalized.y});
    const Vec2 bottomRight =
        normalizedToScreen(Vec2{
            normalized.x + normalized.width,
            normalized.y + normalized.height,
        });

    return RectF{
        .x = topLeft.x,
        .y = topLeft.y,
        .width = bottomRight.x - topLeft.x,
        .height = bottomRight.y - topLeft.y,
    };
}

} // namespace reg::video
