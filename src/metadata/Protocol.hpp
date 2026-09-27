#pragma once

#include "media/FrameIdentity.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace reg::metadata {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxTargetsPerFrame = 32;
inline constexpr std::size_t kWireHeaderSize = 52;
inline constexpr std::size_t kWireTargetSize = 32;
inline constexpr std::size_t kWireCrcSize = 4;
inline constexpr std::size_t kMaxFrameMetadataDatagramSize =
    kWireHeaderSize + kMaxTargetsPerFrame * kWireTargetSize + kWireCrcSize;

enum class PacketType : std::uint16_t {
    FrameMetadata = 1,
    Event = 2,
    Heartbeat = 3,
};

struct NormalizedBox {
    float x{};
    float y{};
    float width{};
    float height{};
};

struct TargetMetadata {
    std::uint64_t id{};
    std::uint16_t classId{};
    std::uint16_t flags{};
    float confidence{};
    NormalizedBox bbox{};
};

struct FrameMetadata {
    media::FrameKey key{};
    std::uint32_t sequence{};
    std::uint64_t cvBeginNs{};
    std::uint64_t cvEndNs{};
    std::uint16_t flags{};
    std::vector<TargetMetadata> targets;
    std::chrono::steady_clock::time_point receivedAt{};
};

enum class ProtocolError {
    PacketTooSmall,
    PacketTooLarge,
    BadMagic,
    UnsupportedVersion,
    UnsupportedType,
    SizeMismatch,
    InvalidTargetCount,
    InvalidTiming,
    InvalidFloat,
    InvalidConfidence,
    InvalidBoundingBox,
    CrcMismatch,
};

std::vector<std::uint8_t> encodeFrameMetadata(const FrameMetadata& metadata);

std::expected<FrameMetadata, ProtocolError> decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt =
        std::chrono::steady_clock::now());

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;
const char* protocolErrorString(ProtocolError error) noexcept;

} // namespace reg::metadata
