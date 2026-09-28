#pragma once

#include "video/VideoTransform.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace reg::render {

struct Color {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
    float a{1.0F};

    auto operator<=>(const Color&) const = default;
};

enum class LinePattern {
    Solid,
    Dashed,
    DashDot,
};

struct LineStyle {
    Color color{};
    float thickness{1.0F};
    LinePattern pattern{LinePattern::Solid};
    float dashLengthPx{8.0F};
    float gapLengthPx{4.0F};
};

struct LinePrimitive {
    video::Vec2 from{};
    video::Vec2 to{};
    LineStyle style{};
};

struct PolylinePrimitive {
    std::vector<video::Vec2> points;
    LineStyle style{};
    bool closed{false};
};

struct RectPrimitive {
    video::RectF rect{};
    LineStyle style{};
};

struct FilledRectPrimitive {
    video::RectF rect{};
    Color color{};
};

struct CirclePrimitive {
    video::Vec2 center{};
    float radiusPx{};
    LineStyle style{};
};

struct CrosshairPrimitive {
    video::Vec2 center{};
    float armLengthPx{8.0F};
    LineStyle style{};
};

struct TextPrimitive {
    video::Vec2 position{};
    std::string utf8;
    Color textColor{};
    Color backgroundColor{0.0F, 0.0F, 0.0F, 0.7F};
    float fontSizePx{18.0F};
    float paddingPx{4.0F};
};

using OverlayPrimitive = std::variant<
    LinePrimitive,
    PolylinePrimitive,
    RectPrimitive,
    FilledRectPrimitive,
    CirclePrimitive,
    CrosshairPrimitive,
    TextPrimitive>;

struct OverlayScene {
    std::vector<OverlayPrimitive> primitives;

    void clear() {
        primitives.clear();
    }

    bool empty() const noexcept {
        return primitives.empty();
    }
};

} // namespace reg::render
