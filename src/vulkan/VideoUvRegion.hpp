#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace reg::vulkan {

// The Vulkan image extent is the coded/aligned frame size, not necessarily
// the visible AVFrame size. H.264 1920x1080, for example, often uses a
// 1920x1088 decode surface. Sampling [0,1] would display the eight padding
// rows and compress actual picture content vertically relative to bbox OSD.
//
// FFmpeg's av_frame_apply_cropping() applies right/bottom cropping to
// AVFrame::width/height for hardware pixel formats, but leaves crop_left/top
// as offsets (it cannot offset VkImage pointers). Therefore:
//   source UV min = crop_left/top / coded dimensions
//   source UV max = AVFrame::width/height / coded dimensions
//   visible dimensions = AVFrame dimensions - crop_left/top
struct VideoUvRegion {
    std::array<float, 4> uvRect{}; // u_min, v_min, u_max, v_max
    std::uint32_t visibleWidth{};
    std::uint32_t visibleHeight{};
};

inline VideoUvRegion computeVideoUvRegion(
    int decodedWidth,
    int decodedHeight,
    int surfaceWidth,
    int surfaceHeight,
    std::size_t cropLeft = 0,
    std::size_t cropTop = 0) {
    if (decodedWidth <= 0 || decodedHeight <= 0 ||
        surfaceWidth <= 0 || surfaceHeight <= 0 ||
        decodedWidth > surfaceWidth || decodedHeight > surfaceHeight ||
        cropLeft >= static_cast<std::size_t>(decodedWidth) ||
        cropTop >= static_cast<std::size_t>(decodedHeight)) {
        throw std::invalid_argument(
            "Video UV region is incompatible with the decoded Vulkan surface");
    }

    return VideoUvRegion{
        .uvRect = {
            static_cast<float>(cropLeft) / static_cast<float>(surfaceWidth),
            static_cast<float>(cropTop) / static_cast<float>(surfaceHeight),
            static_cast<float>(decodedWidth) / static_cast<float>(surfaceWidth),
            static_cast<float>(decodedHeight) / static_cast<float>(surfaceHeight),
        },
        .visibleWidth = static_cast<std::uint32_t>(
            static_cast<std::size_t>(decodedWidth) - cropLeft),
        .visibleHeight = static_cast<std::uint32_t>(
            static_cast<std::size_t>(decodedHeight) - cropTop),
    };
}

} // namespace reg::vulkan
