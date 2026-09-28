#include "media/FrameIdentity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace reg::media {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'R', 'G', 'F', '1'};

void writeLe16(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint16_t value) noexcept {
    bytes[offset] =
        static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] =
        static_cast<std::uint8_t>(
            (value >> 8U) & 0xffU);
}

void writeLe64(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

std::uint16_t readLe16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(
                   bytes[offset + 1]) << 8U);
}

std::uint64_t readLe64(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

} // namespace

std::array<std::uint8_t, kFrameIdentityPayloadSize>
encodeFrameIdentityPayload(
    const SourceFrameIdentity& identity) noexcept {
    std::array<std::uint8_t, kFrameIdentityPayloadSize>
        payload{};

    std::copy(
        kMagic.begin(),
        kMagic.end(),
        payload.begin());

    const std::span<std::uint8_t> bytes(payload);

    writeLe16(
        bytes,
        4,
        kFrameIdentityPayloadVersion);
    writeLe16(
        bytes,
        6,
        static_cast<std::uint16_t>(
            kFrameIdentityPayloadSize));

    writeLe64(
        bytes,
        8,
        identity.key.streamEpoch);
    writeLe64(
        bytes,
        16,
        identity.key.frameId);
    writeLe64(
        bytes,
        24,
        identity.sourceTimeNs);

    return payload;
}

std::optional<SourceFrameIdentity>
decodeFrameIdentityPayload(
    std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < kFrameIdentityPayloadSize) {
        return std::nullopt;
    }

    if (!std::equal(
            kMagic.begin(),
            kMagic.end(),
            payload.begin())) {
        return std::nullopt;
    }

    const std::uint16_t version =
        readLe16(payload, 4);
    const std::uint16_t payloadSize =
        readLe16(payload, 6);

    if (version != kFrameIdentityPayloadVersion ||
        payloadSize < kFrameIdentityPayloadSize ||
        payload.size() < payloadSize) {
        return std::nullopt;
    }

    return SourceFrameIdentity{
        .key = FrameKey{
            .streamEpoch = readLe64(payload, 8),
            .frameId = readLe64(payload, 16),
        },
        .sourceTimeNs = readLe64(payload, 24),
    };
}

} // namespace reg::media
