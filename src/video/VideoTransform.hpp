#pragma once

#include <compare>
#include <cstdint>

namespace reg::video {

struct Vec2 {
    float x{};
    float y{};

    auto operator<=>(const Vec2&) const = default;
};

struct RectF {
    float x{};
    float y{};
    float width{};
    float height{};

    auto operator<=>(const RectF&) const = default;
};

class VideoTransform final {
public:
    VideoTransform(
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t targetWidth,
        std::uint32_t targetHeight);

    void setZoom(float zoom);
    float zoom() const noexcept { return zoom_; }

    // Pan is expressed in normalized source coordinates relative to the
    // unzoomed source. Positive X moves the viewed source region right.
    void setPan(Vec2 pan) noexcept { pan_ = pan; }
    Vec2 pan() const noexcept { return pan_; }

    RectF viewport() const noexcept { return viewport_; }

    Vec2 normalizedToScreen(Vec2 normalized) const noexcept;
    RectF normalizedToScreen(RectF normalized) const noexcept;

private:
    void recomputeViewport();

    std::uint32_t sourceWidth_{};
    std::uint32_t sourceHeight_{};
    std::uint32_t targetWidth_{};
    std::uint32_t targetHeight_{};

    float zoom_{1.0F};
    Vec2 pan_{};
    RectF viewport_{};
};

} // namespace reg::video
