#pragma once

#include "metadata/FrameMetadata.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace reg::metadata::wire {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 52;
inline constexpr std::size_t kTargetSize = 32;
inline constexpr std::size_t kCrcSize = 4;
inline constexpr std::size_t kMaxDatagramSize = 1400;
inline constexpr std::size_t kMaxTargets = 32;

enum class MessageType : std::uint16_t {
    FrameMetadata = 1,
};

enum class DecodeError {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    UnsupportedVersion,
    UnsupportedMessageType,
    SizeMismatch,
    TooManyTargets,
    BadCrc,
    NonFiniteNumber,
    InvalidConfidence,
    InvalidBoundingBox,
    InvalidTiming,
};

struct DecodeResult {
    FrameMetadataPtr metadata;
    DecodeError error{DecodeError::None};

    explicit operator bool() const noexcept {
        return metadata != nullptr && error == DecodeError::None;
    }
};

std::vector<std::uint8_t> encodeFrameMetadata(const FrameMetadata& metadata);

DecodeResult decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt);

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;

const char* decodeErrorName(DecodeError error) noexcept;

} // namespace reg::metadata::wire
