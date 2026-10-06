#pragma once

#include "metadata/FrameMetadata.hpp"
#include "metadata/TrackHistory.hpp"
#include "render/OverlayPrimitives.hpp"
#include "video/VideoTransform.hpp"

namespace reg::render {

struct TargetOverlayStyle {
    Color detectorStrokeColor{0.1F, 1.0F, 0.1F, 1.0F};
    Color detectorFillColor{0.1F, 1.0F, 0.1F, 0.10F};
    Color propagatedStrokeColor{1.0F, 0.72F, 0.12F, 1.0F};
    Color propagatedFillColor{1.0F, 0.72F, 0.12F, 0.08F};
    Color trailColor{0.1F, 1.0F, 0.1F, 0.75F};
    Color textColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color textBackgroundColor{0.0F, 0.0F, 0.0F, 0.70F};

    float bboxThicknessPx{2.0F};
    float trailThicknessPx{1.5F};
    float crosshairArmPx{7.0F};
    float fontSizePx{18.0F};
    float labelPaddingPx{4.0F};
    float labelGapPx{4.0F};

    LinePattern detectorPattern{LinePattern::Solid};
    LinePattern propagatedPattern{LinePattern::Dashed};
    LinePattern trailPattern{LinePattern::Solid};
};

class TargetOverlayBuilder final {
public:
    explicit TargetOverlayBuilder(TargetOverlayStyle style = {});

    void setMasterAlpha(float alpha);
    float masterAlpha() const noexcept { return masterAlpha_; }

    OverlayScene build(
        const metadata::FrameMetadata& metadata,
        const metadata::TrackHistory& history,
        const video::VideoTransform& transform) const;

private:
    Color applyMasterAlpha(Color color) const noexcept;

    TargetOverlayStyle style_{};
    float masterAlpha_{1.0F};
};

} // namespace reg::render
