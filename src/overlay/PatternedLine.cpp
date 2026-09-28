#include "overlay/PatternedLine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace reg::overlay {
namespace {

struct PatternInterval {
    float length{};
    bool visible{};
};

struct PatternDescription {
    std::array<PatternInterval, 4> intervals{};
    std::size_t count{};
    float period{};
};

PatternDescription describePattern(const StrokePattern& pattern) {
    const auto positiveFinite = [](float value) {
        return std::isfinite(value) && value > 0.0F;
    };

    switch (pattern.pattern) {
    case LinePattern::Solid:
        return PatternDescription{};

    case LinePattern::Dashed:
        if (!positiveFinite(pattern.dashLength) ||
            !positiveFinite(pattern.gapLength)) {
            throw std::invalid_argument(
                "Dashed pattern lengths must be finite and positive");
        }

        return PatternDescription{
            .intervals = {{
                {pattern.dashLength, true},
                {pattern.gapLength, false},
                {},
                {},
            }},
            .count = 2,
            .period = pattern.dashLength + pattern.gapLength,
        };

    case LinePattern::DashDot:
        if (!positiveFinite(pattern.dashLength) ||
            !positiveFinite(pattern.gapLength) ||
            !positiveFinite(pattern.dotLength)) {
            throw std::invalid_argument(
                "Dash-dot pattern lengths must be finite and positive");
        }

        return PatternDescription{
            .intervals = {{
                {pattern.dashLength, true},
                {pattern.gapLength, false},
                {pattern.dotLength, true},
                {pattern.gapLength, false},
            }},
            .count = 4,
            .period =
                pattern.dashLength +
                pattern.gapLength +
                pattern.dotLength +
                pattern.gapLength,
        };
    }

    throw std::invalid_argument("Unknown line pattern");
}

float normalizedPhase(float phase, float period) {
    if (!std::isfinite(phase)) {
        throw std::invalid_argument("Line-pattern phase must be finite");
    }

    float normalized = std::fmod(phase, period);
    if (normalized < 0.0F) {
        normalized += period;
    }
    return normalized;
}

std::pair<std::size_t, float> locateInterval(
    const PatternDescription& pattern,
    float positionInPeriod) {
    float cursor = 0.0F;

    for (std::size_t index = 0; index < pattern.count; ++index) {
        const float end = cursor + pattern.intervals[index].length;
        if (positionInPeriod < end || index + 1 == pattern.count) {
            return {index, end - positionInPeriod};
        }
        cursor = end;
    }

    return {0, pattern.intervals[0].length};
}

void tessellateSegment(
    Vec2f start,
    Vec2f end,
    const StrokePattern& stroke,
    const PatternDescription& pattern,
    float& accumulatedDistance,
    std::vector<LineSegment>& output) {
    const Vec2f delta = end - start;
    const float segmentLength = length(delta);

    if (!std::isfinite(segmentLength)) {
        throw std::invalid_argument("Line geometry contains a non-finite point");
    }
    if (segmentLength <= 0.0F) {
        return;
    }

    if (stroke.pattern == LinePattern::Solid) {
        output.push_back({start, end});
        accumulatedDistance += segmentLength;
        return;
    }

    const Vec2f direction = delta / segmentLength;
    const float phase = normalizedPhase(stroke.phase, pattern.period);

    float localDistance = 0.0F;
    constexpr float kEpsilon = 0.0001F;

    while (localDistance < segmentLength - kEpsilon) {
        const float globalPatternDistance =
            accumulatedDistance + localDistance + phase;

        float positionInPeriod =
            std::fmod(globalPatternDistance, pattern.period);
        if (positionInPeriod < 0.0F) {
            positionInPeriod += pattern.period;
        }

        const auto [intervalIndex, intervalRemaining] =
            locateInterval(pattern, positionInPeriod);

        const float remainingOnSegment = segmentLength - localDistance;
        const float chunk =
            std::min(intervalRemaining, remainingOnSegment);

        if (chunk <= kEpsilon) {
            localDistance += kEpsilon;
            continue;
        }

        if (pattern.intervals[intervalIndex].visible) {
            output.push_back(LineSegment{
                .start = start + direction * localDistance,
                .end = start + direction * (localDistance + chunk),
            });
        }

        localDistance += chunk;
    }

    accumulatedDistance += segmentLength;
}

} // namespace

std::vector<LineSegment> tessellatePatternedLine(
    Vec2f start,
    Vec2f end,
    const StrokePattern& pattern) {
    if (!finite(start) || !finite(end)) {
        throw std::invalid_argument("Line endpoints must be finite");
    }

    const PatternDescription description = describePattern(pattern);

    std::vector<LineSegment> result;
    float accumulatedDistance = 0.0F;
    tessellateSegment(
        start,
        end,
        pattern,
        description,
        accumulatedDistance,
        result);
    return result;
}

std::vector<LineSegment> tessellatePatternedPolyline(
    std::span<const Vec2f> points,
    const StrokePattern& pattern) {
    const PatternDescription description = describePattern(pattern);

    std::vector<LineSegment> result;
    if (points.size() < 2) {
        return result;
    }

    float accumulatedDistance = 0.0F;

    for (std::size_t index = 1; index < points.size(); ++index) {
        if (!finite(points[index - 1]) || !finite(points[index])) {
            throw std::invalid_argument("Polyline points must be finite");
        }

        tessellateSegment(
            points[index - 1],
            points[index],
            pattern,
            description,
            accumulatedDistance,
            result);
    }

    return result;
}

} // namespace reg::overlay
