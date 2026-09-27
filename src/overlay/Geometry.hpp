#pragma once

#include <cmath>

namespace reg::overlay {

struct Vec2f {
    float x{};
    float y{};

    friend constexpr Vec2f operator+(Vec2f left, Vec2f right) noexcept {
        return {left.x + right.x, left.y + right.y};
    }

    friend constexpr Vec2f operator-(Vec2f left, Vec2f right) noexcept {
        return {left.x - right.x, left.y - right.y};
    }

    friend constexpr Vec2f operator*(Vec2f value, float scale) noexcept {
        return {value.x * scale, value.y * scale};
    }

    friend constexpr Vec2f operator/(Vec2f value, float scale) noexcept {
        return {value.x / scale, value.y / scale};
    }

    auto operator<=>(const Vec2f&) const = default;
};

struct RectF {
    float x{};
    float y{};
    float width{};
    float height{};

    constexpr float left() const noexcept { return x; }
    constexpr float top() const noexcept { return y; }
    constexpr float right() const noexcept { return x + width; }
    constexpr float bottom() const noexcept { return y + height; }

    auto operator<=>(const RectF&) const = default;
};

inline float length(Vec2f vector) noexcept {
    return std::hypot(vector.x, vector.y);
}

inline Vec2f normalized(Vec2f vector) noexcept {
    const float magnitude = length(vector);
    return magnitude > 0.0F ? vector / magnitude : Vec2f{};
}

inline bool finite(Vec2f value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace reg::overlay
