#pragma once

#include "overlay/Geometry.hpp"

namespace reg::video {

enum class FitMode {
    Contain,
    Cover,
};

struct VideoTransformConfig {
    int sourceWidth{};
    int sourceHeight{};

    float targetWidth{};
    float targetHeight{};

    FitMode fitMode{FitMode::Contain};

    // Zoom is applied after the fit transform. 1.0 means fit exactly.
    float zoom{1.0F};

    // The source-space point that maps to the center of the target viewport.
    // (0.5, 0.5) is the default centered view.
    overlay::Vec2f centerNormalized{0.5F, 0.5F};
};

class VideoTransform final {
public:
    explicit VideoTransform(VideoTransformConfig config);

    const VideoTransformConfig& config() const noexcept { return config_; }

    float scale() const noexcept { return scale_; }

    overlay::Vec2f normalizedToScreen(overlay::Vec2f normalized) const noexcept;
    overlay::Vec2f screenToNormalized(overlay::Vec2f screen) const noexcept;

    overlay::RectF normalizedRectToScreen(overlay::RectF normalizedRect) const noexcept;
    overlay::RectF sourceContentRectOnScreen() const noexcept;

private:
    VideoTransformConfig config_{};
    float scale_{};
    overlay::Vec2f targetCenter_{};
    overlay::Vec2f sourceCenterPixels_{};
};

} // namespace reg::video
