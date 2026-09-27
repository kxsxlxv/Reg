#pragma once

extern "C" {
#include <libavutil/error.h>
}

#include <array>
#include <stdexcept>
#include <string>

namespace reg::media {

inline std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

[[noreturn]] inline void throwFfmpegError(const char* operation, int errorCode) {
    throw std::runtime_error(
        std::string(operation) + " failed: " + ffmpegErrorString(errorCode) +
        " (" + std::to_string(errorCode) + ")");
}

inline void checkFfmpeg(int result, const char* operation) {
    if (result < 0) {
        throwFfmpegError(operation, result);
    }
}

} // namespace reg::media
