#pragma once

#include "overlay/Geometry.hpp"

#include <span>
#include <vector>

namespace reg::overlay {

enum class LinePattern {
    Solid,
    Dashed,
    DashDot,
};

struct StrokePattern {
    LinePattern pattern{LinePattern::Solid};

    // All lengths are screen pixels.
    float dashLength{8.0F};
    float gapLength{4.0F};
    float dotLength{2.0F};

    // Screen-pixel distance into the repeating pattern.
    float phase{0.0F};
};

struct LineSegment {
    Vec2f start{};
    Vec2f end{};
};

std::vector<LineSegment> tessellatePatternedLine(
    Vec2f start,
    Vec2f end,
    const StrokePattern& pattern);

std::vector<LineSegment> tessellatePatternedPolyline(
    std::span<const Vec2f> points,
    const StrokePattern& pattern);

} // namespace reg::overlay
