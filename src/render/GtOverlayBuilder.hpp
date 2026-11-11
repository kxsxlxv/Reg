#pragma once

#include "media/GtSei.hpp"
#include "render/OverlayPrimitives.hpp"
#include "video/VideoTransform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace reg::render {

// Local Raw OSD only. No history, prediction, packet matching, or GPU upload
// of source pixel data. The same aspect-fit transform as VideoRenderer's
// videoViewport() maps half-open video-pixel coordinates to screen coordinates.
class GtOverlayBuilder final {
public:
    OverlayScene build(
        const media::GtMetadata& gt,
        std::uint32_t targetWidth,
        std::uint32_t targetHeight,
        bool showIds) const {
        OverlayScene result;
        if (gt.status != media::GtStatus::Ready ||
            gt.videoWidth == 0U || gt.videoHeight == 0U ||
            targetWidth == 0U || targetHeight == 0U) return result;

        const video::VideoTransform transform(
            gt.videoWidth, gt.videoHeight, targetWidth, targetHeight);
        const video::RectF viewport = transform.viewport();
        const float left = viewport.x;
        const float top = viewport.y;
        const float right = left + viewport.width;
        const float bottom = top + viewport.height;
        result.primitives.reserve(gt.objects.size() * (showIds ? 2U : 1U) + 2U);
        result.primitives.emplace_back(ClipPushPrimitive{viewport});
        constexpr Color stroke{1.0F, 0.24F, 0.82F, 1.0F};
        constexpr Color background{0.05F, 0.02F, 0.10F, 0.86F};

        for (const media::GtObject& object : gt.objects) {
            if (!object.visible || !object.validBox) continue;
            const auto& box = object.xyxyVideo;
            // Preserve continuous xyxy end coordinates, not width/height.
            const video::RectF normalized{
                static_cast<float>(box[0] / static_cast<double>(gt.videoWidth)),
                static_cast<float>(box[1] / static_cast<double>(gt.videoHeight)),
                static_cast<float>((box[2] - box[0]) / static_cast<double>(gt.videoWidth)),
                static_cast<float>((box[3] - box[1]) / static_cast<double>(gt.videoHeight)),
            };
            const video::RectF mapped = transform.normalizedToScreen(normalized);
            if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y) ||
                !std::isfinite(mapped.width) || !std::isfinite(mapped.height)) continue;

            const float x0 = std::clamp(mapped.x, left, right);
            const float y0 = std::clamp(mapped.y, top, bottom);
            const float x1 = std::clamp(mapped.x + mapped.width, left, right);
            const float y1 = std::clamp(mapped.y + mapped.height, top, bottom);
            if (x1 <= x0 || y1 <= y0) continue;
            const video::RectF clipped{
                .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
            result.primitives.emplace_back(RectPrimitive{
                .rect = clipped,
                .style = LineStyle{.color = stroke, .thickness = 1.5F},
            });

            if (showIds && !object.id.empty()) {
                constexpr float fontSize = 16.0F;
                constexpr float pad = 3.0F;
                constexpr float gap = 4.0F;
                const float labelHeight = fontSize + pad * 2.0F;
                const float labelWidth = static_cast<float>(object.id.size()) *
                    fontSize * 0.60F + pad * 2.0F;
                const float labelX = std::clamp(
                    clipped.x, left, std::max(left, right - labelWidth));
                const float above = clipped.y - labelHeight - gap;
                const float below = clipped.y + clipped.height + gap;
                float labelY = 0.0F;
                if (above >= top) labelY = above;
                else if (below + labelHeight <= bottom) labelY = below;
                else continue;
                result.primitives.emplace_back(TextPrimitive{
                    .position = video::Vec2{labelX, labelY},
                    .utf8 = object.id,
                    .textColor = Color{1.0F, 1.0F, 1.0F, 1.0F},
                    .backgroundColor = background,
                    .fontSizePx = fontSize,
                    .paddingPx = pad,
                });
            }
        }
        result.primitives.emplace_back(ClipPopPrimitive{});
        return result;
    }
};

} // namespace reg::render
