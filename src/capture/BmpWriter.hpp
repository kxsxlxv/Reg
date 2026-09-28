#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace reg::capture {

enum class PixelOrder {
    Bgra8,
    Rgba8,
};

// Writes a lossless 32-bit Windows BMP. Input rows are tightly packed and use
// top-left origin; BMP rows are emitted bottom-up.
void writeBmp32(
    const std::filesystem::path& path,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> pixels,
    PixelOrder pixelOrder);

} // namespace reg::capture
