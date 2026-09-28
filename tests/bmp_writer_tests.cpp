#include "capture/BmpWriter.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

std::uint32_t readLe32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset) {
    return
        static_cast<std::uint32_t>(
            bytes[offset]) |
        (static_cast<std::uint32_t>(
             bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(
             bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(
             bytes[offset + 3]) << 24U);
}

void bmpHeaderAndChannelOrderAreStable() {
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    const auto path =
        std::filesystem::temp_directory_path() /
        ("reg_bmp_test_" + unique + ".bmp");

    // Top row: red, green. Bottom row: blue, white. Input is RGBA.
    constexpr std::array<std::uint8_t, 16>
        rgba{
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 255, 255,
            255, 255, 255, 255,
        };

    try {
        reg::capture::writeBmp32(
            path,
            2,
            2,
            rgba,
            reg::capture::PixelOrder::Rgba8);

        std::ifstream stream(
            path,
            std::ios::binary |
                std::ios::ate);

        require(
            static_cast<bool>(stream),
            "BMP file was not created");

        const auto size =
            stream.tellg();

        require(
            size == 70,
            "unexpected BMP file size");

        std::vector<std::uint8_t>
            bytes(
                static_cast<std::size_t>(
                    size));

        stream.seekg(0);
        stream.read(
            reinterpret_cast<char*>(
                bytes.data()),
            size);

        require(
            bytes[0] == 'B' &&
                bytes[1] == 'M',
            "BMP signature mismatch");
        require(
            readLe32(bytes, 10) == 54,
            "BMP pixel offset mismatch");
        require(
            readLe32(bytes, 18) == 2 &&
                readLe32(bytes, 22) == 2,
            "BMP dimensions mismatch");

        // First pixel on disk is bottom-left blue, encoded BGRA.
        require(
            bytes[54] == 255 &&
                bytes[55] == 0 &&
                bytes[56] == 0 &&
                bytes[57] == 255,
            "BMP bottom-up/channel conversion mismatch");

        // Third pixel is top-left red, encoded BGRA.
        require(
            bytes[62] == 0 &&
                bytes[63] == 0 &&
                bytes[64] == 255 &&
                bytes[65] == 255,
            "BMP top-row conversion mismatch");
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }

    std::filesystem::remove(path);
}

} // namespace

int main() {
    try {
        bmpHeaderAndChannelOrderAreStable();

        std::cout
            << "bmp_writer_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "bmp_writer_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
