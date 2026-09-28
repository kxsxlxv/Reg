#include "video/VideoTransform.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace reg::video {

VideoTransform::VideoTransform(VideoTransformConfig config)
    : config_(config) {
    if (config_.sourceWidth <= 0 || config_.sourceHeight <= 0) {
        throw std::invalid_argument("VideoTransform source dimensions must be positive");
    }
    if (!std::isfinite(config_.targetWidth) ||
        !std::isfinite(config_.targetHeight) ||
        config_.targetWidth <= 0.0F ||
        config_.targetHeight <= 0.0F) {
        throw std::invalid_argument("VideoTransform target dimensions must be finite and positive");
    }
    if (!std::isfinite(config_.zoom) || config_.zoom <= 0.0F) {
        throw std::invalid_argument("VideoTransform zoom must be finite and positive");
    }
    if (!overlay::finite(config_.centerNormalized)) {
        throw std::invalid_argument("VideoTransform center must be finite");
    }

    const float sourceWidth = static_cast<float>(config_.sourceWidth);
    const float sourceHeight = static_cast<float>(config_.sourceHeight);

    const float scaleX = config_.targetWidth / sourceWidth;
    const float scaleY = config_.targetHeight / sourceHeight;

    const float fitScale =
        config_.fitMode == FitMode::Contain
            ? std::min(scaleX, scaleY)
            : std::max(scaleX, scaleY);

    scale_ = fitScale * config_.zoom;
    targetCenter_ = {
        config_.targetWidth * 0.5F,
        config_.targetHeight * 0.5F,
    };
    sourceCenterPixels_ = {
        config_.centerNormalized.x * sourceWidth,
        config_.centerNormalized.y * sourceHeight,
    };
}

overlay::Vec2f VideoTransform::normalizedToScreen(
    overlay::Vec2f normalized) const noexcept {
    const overlay::Vec2f sourcePixels{
        normalized.x * static_cast<float>(config_.sourceWidth),
        normalized.y * static_cast<float>(config_.sourceHeight),
    };

    return targetCenter_ + (sourcePixels - sourceCenterPixels_) * scale_;
}

overlay::Vec2f VideoTransform::screenToNormalized(
    overlay::Vec2f screen) const noexcept {
    const overlay::Vec2f sourcePixels =
        sourceCenterPixels_ + (screen - targetCenter_) / scale_;

    return {
        sourcePixels.x / static_cast<float>(config_.sourceWidth),
        sourcePixels.y / static_cast<float>(config_.sourceHeight),
    };
}

overlay::RectF VideoTransform::normalizedRectToScreen(
    overlay::RectF normalizedRect) const noexcept {
    const overlay::Vec2f topLeft =
        normalizedToScreen({normalizedRect.x, normalizedRect.y});
    const overlay::Vec2f bottomRight =
        normalizedToScreen({
            normalizedRect.x + normalizedRect.width,
            normalizedRect.y + normalizedRect.height,
        });

    return {
        .x = topLeft.x,
        .y = topLeft.y,
        .width = bottomRight.x - topLeft.x,
        .height = bottomRight.y - topLeft.y,
    };
}

overlay::RectF VideoTransform::sourceContentRectOnScreen() const noexcept {
    return normalizedRectToScreen({
        .x = 0.0F,
        .y = 0.0F,
        .width = 1.0F,
        .height = 1.0F,
    });
}

} // namespace reg::video
