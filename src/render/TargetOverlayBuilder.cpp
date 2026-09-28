#include "render/TargetOverlayBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reg::render {

TargetOverlayBuilder::TargetOverlayBuilder(
    TargetOverlayStyle style)
    : style_(style) {}

void TargetOverlayBuilder::setMasterAlpha(float alpha) {
    if (!std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
        throw std::invalid_argument("overlay master alpha must be in [0,1]");
    }
    masterAlpha_ = alpha;
}

Color TargetOverlayBuilder::applyMasterAlpha(
    Color color) const noexcept {
    color.a *= masterAlpha_;
    return color;
}

OverlayScene TargetOverlayBuilder::build(
    const metadata::FrameMetadata& metadata,
    const metadata::TrackHistory& history,
    const video::VideoTransform& transform) const {
    OverlayScene scene;

    // Typical load is <= 10 targets. Reserve enough for fill + bbox +
    // crosshair + trail + label without reallocating every frame.
    scene.primitives.reserve(metadata.targets.size() * 5U);

    for (const metadata::TargetMetadata& target : metadata.targets) {
        const video::RectF normalizedRect{
            .x = target.bbox.x,
            .y = target.bbox.y,
            .width = target.bbox.width,
            .height = target.bbox.height,
        };
        const video::RectF screenRect =
            transform.normalizedToScreen(normalizedRect);

        scene.primitives.emplace_back(FilledRectPrimitive{
            .rect = screenRect,
            .color = applyMasterAlpha(style_.fillColor),
        });

        scene.primitives.emplace_back(RectPrimitive{
            .rect = screenRect,
            .style = LineStyle{
                .color = applyMasterAlpha(style_.strokeColor),
                .thickness = style_.bboxThicknessPx,
                .pattern = style_.bboxPattern,
            },
        });

        const video::Vec2 center{
            .x = screenRect.x + screenRect.width * 0.5F,
            .y = screenRect.y + screenRect.height * 0.5F,
        };

        scene.primitives.emplace_back(CrosshairPrimitive{
            .center = center,
            .armLengthPx = style_.crosshairArmPx,
            .style = LineStyle{
                .color = applyMasterAlpha(style_.strokeColor),
                .thickness = style_.bboxThicknessPx,
                .pattern = LinePattern::Solid,
            },
        });

        const auto trackPoints = history.points(target.id);
        if (trackPoints.size() >= 2) {
            PolylinePrimitive trail;
            trail.style = LineStyle{
                .color = applyMasterAlpha(style_.trailColor),
                .thickness = style_.trailThicknessPx,
                .pattern = style_.trailPattern,
            };
            trail.points.reserve(trackPoints.size());

            for (const metadata::TrackPoint& point : trackPoints) {
                trail.points.push_back(
                    transform.normalizedToScreen(
                        point.normalizedPosition));
            }

            scene.primitives.emplace_back(std::move(trail));
        }

        char label[128]{};
        std::snprintf(
            label,
            sizeof(label),
            "ID %llu  C%u  %.0f%%",
            static_cast<unsigned long long>(target.id),
            static_cast<unsigned>(target.classId),
            static_cast<double>(target.confidence * 100.0F));

        scene.primitives.emplace_back(TextPrimitive{
            .position = video::Vec2{
                .x = screenRect.x,
                .y = screenRect.y,
            },
            .utf8 = std::string(label),
            .textColor = applyMasterAlpha(style_.textColor),
            .backgroundColor =
                applyMasterAlpha(style_.textBackgroundColor),
            .fontSizePx = style_.fontSizePx,
            .paddingPx = style_.labelPaddingPx,
        });
    }

    return scene;
}

} // namespace reg::render
