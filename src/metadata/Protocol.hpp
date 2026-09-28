#pragma once

#include "metadata/FrameMetadata.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace reg::metadata::protocol {

inline constexpr std::uint16_t kLegacyVersion = 1;
inline constexpr std::uint16_t kVersion = 2;

inline constexpr std::size_t kHeaderSize = 52;
inline constexpr std::size_t kLegacyTargetSize = 32;
inline constexpr std::size_t kTargetV2BaseSize = 36;
inline constexpr std::size_t kContourPointSize = 8;
inline constexpr std::size_t kCrcSize = 4;

inline constexpr std::size_t kMaxDatagramSize = 1400;
inline constexpr std::size_t kMaxTargets = 32;
inline constexpr std::size_t kMaxContourPointsPerTarget = 64;

enum class MessageType : std::uint16_t {
    FrameMetadata = 1,
    Event = 2,
    Heartbeat = 3,
};

enum class DecodeError {
    None,
    TooSmall,
    BadMagic,
    UnsupportedVersion,
    UnsupportedType,
    SizeMismatch,
    TargetCountTooLarge,
    CrcMismatch,
    InvalidNumber,
    InvalidNormalizedRect,
    ContourCountTooLarge,
    InvalidContour,
};

struct DecodeResult {
    FrameMetadata metadata{};
    DecodeError error{DecodeError::None};

    explicit operator bool() const noexcept {
        return error == DecodeError::None;
    }
};

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;

std::vector<std::uint8_t> encodeFrameMetadata(const FrameMetadata& metadata);

DecodeResult decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt =
        std::chrono::steady_clock::now());

const char* toString(DecodeError error) noexcept;

} // namespace reg::metadata::protocol
