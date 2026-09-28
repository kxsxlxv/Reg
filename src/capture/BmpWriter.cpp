#include "capture/BmpWriter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace reg::capture {
namespace {

void writeLe16(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint16_t value) {
    bytes[offset] =
        static_cast<std::uint8_t>(
            value & 0xffU);
    bytes[offset + 1] =
        static_cast<std::uint8_t>(
            (value >> 8U) & 0xffU);
}

void writeLe32(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

} // namespace

void writeBmp32(
    const std::filesystem::path& path,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> pixels,
    PixelOrder pixelOrder) {
    if (width == 0 || height == 0) {
        throw std::invalid_argument(
            "BMP dimensions must be non-zero");
    }

    constexpr std::uint64_t bytesPerPixel = 4;
    const std::uint64_t rowBytes =
        static_cast<std::uint64_t>(width) *
        bytesPerPixel;
    const std::uint64_t pixelBytes =
        rowBytes *
        static_cast<std::uint64_t>(height);

    if (pixelBytes >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::overflow_error(
            "BMP pixel buffer exceeds addressable size");
    }

    if (pixels.size() !=
        static_cast<std::size_t>(
            pixelBytes)) {
        throw std::invalid_argument(
            "BMP pixel buffer size does not match dimensions");
    }

    constexpr std::uint32_t fileHeaderSize = 14;
    constexpr std::uint32_t dibHeaderSize = 40;
    constexpr std::uint32_t pixelOffset =
        fileHeaderSize + dibHeaderSize;

    const std::uint64_t totalSize64 =
        static_cast<std::uint64_t>(
            pixelOffset) +
        pixelBytes;

    if (totalSize64 >
        std::numeric_limits<
            std::uint32_t>::max()) {
        throw std::overflow_error(
            "BMP file exceeds 32-bit BMP size field");
    }

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(
            parent);
    }

    std::ofstream stream(
        path,
        std::ios::binary |
            std::ios::out |
            std::ios::trunc);

    if (!stream) {
        throw std::runtime_error(
            "Cannot open screenshot output: " +
            path.string());
    }

    std::array<std::uint8_t, pixelOffset>
        header{};

    header[0] = 'B';
    header[1] = 'M';

    writeLe32(
        header,
        2,
        static_cast<std::uint32_t>(
            totalSize64));
    writeLe32(
        header,
        10,
        pixelOffset);

    writeLe32(
        header,
        14,
        dibHeaderSize);
    writeLe32(
        header,
        18,
        width);
    writeLe32(
        header,
        22,
        height);
    writeLe16(
        header,
        26,
        1);
    writeLe16(
        header,
        28,
        32);
    // compression=BI_RGB (0). For 32-bit BI_RGB, byte order on disk is BGRA.
    writeLe32(
        header,
        34,
        static_cast<std::uint32_t>(
            pixelBytes));

    stream.write(
        reinterpret_cast<const char*>(
            header.data()),
        static_cast<std::streamsize>(
            header.size()));

    if (!stream) {
        throw std::runtime_error(
            "Failed to write BMP header");
    }

    std::vector<std::uint8_t>
        convertedRow(
            static_cast<std::size_t>(
                rowBytes));

    for (std::uint32_t outputRow = 0;
         outputRow < height;
         ++outputRow) {
        const std::uint32_t sourceRow =
            height - 1U - outputRow;

        const auto row =
            pixels.subspan(
                static_cast<std::size_t>(
                    static_cast<std::uint64_t>(
                        sourceRow) *
                    rowBytes),
                static_cast<std::size_t>(
                    rowBytes));

        if (pixelOrder ==
            PixelOrder::Bgra8) {
            stream.write(
                reinterpret_cast<const char*>(
                    row.data()),
                static_cast<std::streamsize>(
                    row.size()));
        } else {
            for (std::uint32_t x = 0;
                 x < width;
                 ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(
                        x) * 4U;

                convertedRow[offset + 0] =
                    row[offset + 2];
                convertedRow[offset + 1] =
                    row[offset + 1];
                convertedRow[offset + 2] =
                    row[offset + 0];
                convertedRow[offset + 3] =
                    row[offset + 3];
            }

            stream.write(
                reinterpret_cast<const char*>(
                    convertedRow.data()),
                static_cast<std::streamsize>(
                    convertedRow.size()));
        }

        if (!stream) {
            throw std::runtime_error(
                "Failed to write BMP pixel data");
        }
    }
}

} // namespace reg::capture
