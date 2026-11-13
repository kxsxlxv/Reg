#include "render/TargetOverlayBuilder.hpp"
#include "render/Cvm1SemanticStyle.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace reg::render {
namespace {


struct LabelPlacement {
    video::Vec2 position{};
    bool visible{};
};

LabelPlacement placeLabel(
    const video::RectF& bbox,
    const video::RectF& viewport,
    const TargetOverlayStyle& style,
    std::string_view text) noexcept {
    const float padding = std::max(style.labelPaddingPx, 0.0F);
    const float gap = std::max(style.labelGapPx, 0.0F);
    const float labelHeight = std::max(style.fontSizePx, 1.0F) + padding * 2.0F;

    // Labels are currently ASCII provenance/ID strings. This conservative
    // estimate is used only to keep the background inside the video viewport;
    // ImGui still computes the exact text size when drawing.
    const float estimatedTextWidth =
        static_cast<float>(text.size()) * std::max(style.fontSizePx, 1.0F) * 0.60F;
    const float labelWidth = estimatedTextWidth + padding * 2.0F;

    const float viewportLeft = viewport.x;
    const float viewportTop = viewport.y;
    const float viewportRight = viewport.x + viewport.width;
    const float viewportBottom = viewport.y + viewport.height;

    const float maxX = std::max(viewportLeft, viewportRight - labelWidth);
    const float x = std::clamp(bbox.x, viewportLeft, maxX);

    // Prefer above the bbox. At the top edge flip below it. Never place the
    // annotation over the target itself; if neither vertical side has room,
    // omit the label for that frame instead of obscuring the bbox.
    const float aboveY = bbox.y - labelHeight - gap;
    if (aboveY >= viewportTop) {
        return LabelPlacement{
            .position = video::Vec2{.x = x, .y = aboveY},
            .visible = true,
        };
    }

    const float belowY = bbox.y + bbox.height + gap;
    if (belowY + labelHeight <= viewportBottom) {
        return LabelPlacement{
            .position = video::Vec2{.x = x, .y = belowY},
            .visible = true,
        };
    }

    return {};
}

} // namespace

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
    // TrackHistory remains part of the builder API because the metadata/render
    // pipeline still owns it, but target trajectories are intentionally not
    // rendered. DETR detections and OFA propagated targets use the same compact
    // bbox/crosshair/label presentation.
    static_cast<void>(history);

    OverlayScene scene;

    // Typical load is <= 10 targets. Reserve fill + bbox + crosshair + label.
    scene.primitives.reserve(metadata.targets.size() * 14U);

    const video::RectF viewport = transform.viewport();
    const auto palette = cvm1::current();

    for (const metadata::TargetMetadata& target : metadata.targets) {
        const auto& semantic = palette->style(target.flags);
        if (!semantic.visible) continue;
        const Color strokeColor{semantic.color.r, semantic.color.g,
                                semantic.color.b, semantic.color.a};
        Color fillColor = strokeColor;
        fillColor.a = 0.10F;
        const LinePattern bboxPattern = semantic.pattern == cvm1::Pattern::dashed
            ? LinePattern::Dashed : LinePattern::Solid;

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
            .color = applyMasterAlpha(fillColor),
        });

        const LineStyle semanticLine{
            .color = applyMasterAlpha(strokeColor),
            .thickness = semantic.width,
            .pattern = bboxPattern,
        };
        if (semantic.pattern == cvm1::Pattern::corners) {
            const float arm = std::min({screenRect.width * 0.26F,
                screenRect.height * 0.26F, 22.0F});
            const float x = screenRect.x, y = screenRect.y;
            const float r = x + screenRect.width, b = y + screenRect.height;
            const video::Vec2 a{x,y}, c{r,y}, d{x,b}, e{r,b};
            const video::Vec2 aa{x+arm,y}, ab{x,y+arm};
            const video::Vec2 ca{r-arm,y}, cb{r,y+arm};
            const video::Vec2 da{x+arm,b}, db{x,b-arm};
            const video::Vec2 ea{r-arm,b}, eb{r,b-arm};
            const std::array<std::pair<video::Vec2,video::Vec2>,8> edges{{
                {a,aa}, {a,ab}, {c,ca}, {c,cb},
                {d,da}, {d,db}, {e,ea}, {e,eb}
            }};
            for (const auto& edge : edges) {
                scene.primitives.emplace_back(LinePrimitive{
                    .from = edge.first, .to = edge.second, .style = semanticLine});
            }
        } else {
            scene.primitives.emplace_back(RectPrimitive{
                .rect = screenRect, .style = semanticLine});
        }

        const video::Vec2 center{
            .x = screenRect.x + screenRect.width * 0.5F,
            .y = screenRect.y + screenRect.height * 0.5F,
        };

        scene.primitives.emplace_back(CrosshairPrimitive{
            .center = center,
            .armLengthPx = style_.crosshairArmPx,
            .style = LineStyle{
                .color = applyMasterAlpha(strokeColor),
                .thickness = semantic.width,
                .pattern = LinePattern::Solid,
            },
        });

        const char* source = semantic.label.c_str();
        char label[128]{};
        if (cvm1::isEphemeralId(target.id)) {
            std::snprintf(label, sizeof(label),
                "%s  frame-local  C%u  %.0f%%", source,
                static_cast<unsigned>(target.classId),
                static_cast<double>(target.confidence * 100.0F));
        } else {
            std::snprintf(label, sizeof(label),
                "%s  ID %llu  C%u  %.0f%%", source,
                static_cast<unsigned long long>(target.id),
                static_cast<unsigned>(target.classId),
                static_cast<double>(target.confidence * 100.0F));
        }

        const std::string labelText(label);
        const LabelPlacement placement = placeLabel(
            screenRect,
            viewport,
            style_,
            labelText);

        if (placement.visible) {
            scene.primitives.emplace_back(TextPrimitive{
                .position = placement.position,
                .utf8 = labelText,
                .textColor = applyMasterAlpha(style_.textColor),
                .backgroundColor =
                    applyMasterAlpha(style_.textBackgroundColor),
                .fontSizePx = style_.fontSizePx,
                .paddingPx = style_.labelPaddingPx,
            });
        }
    }

    return scene;
}

} // namespace reg::render
