#include "metadata/Protocol.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace reg::metadata {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'C', 'V', 'M', '1'};
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendFloat(std::vector<std::uint8_t>& out, float value) {
    appendU32(out, std::bit_cast<std::uint32_t>(value));
}

std::uint16_t readU16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

std::uint32_t readU32(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(data[offset + byte]) << (byte * 8U);
    }
    return value;
}

std::uint64_t readU64(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(data[offset + byte]) << (byte * 8U);
    }
    return value;
}

float readFloat(std::span<const std::uint8_t> data, std::size_t offset) {
    return std::bit_cast<float>(readU32(data, offset));
}

bool isNormalized(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

std::expected<void, ProtocolError> validate(const FrameMetadata& metadata) {
    if (metadata.targets.size() > kMaxTargetsPerFrame) {
        return std::unexpected(ProtocolError::InvalidTargetCount);
    }

    if (metadata.cvBeginNs != 0 &&
        metadata.cvEndNs != 0 &&
        metadata.cvEndNs < metadata.cvBeginNs) {
        return std::unexpected(ProtocolError::InvalidTiming);
    }

    for (const auto& target : metadata.targets) {
        if (!std::isfinite(target.confidence)) {
            return std::unexpected(ProtocolError::InvalidFloat);
        }
        if (target.confidence < 0.0F || target.confidence > 1.0F) {
            return std::unexpected(ProtocolError::InvalidConfidence);
        }

        if (!std::isfinite(target.bbox.x) ||
            !std::isfinite(target.bbox.y) ||
            !std::isfinite(target.bbox.width) ||
            !std::isfinite(target.bbox.height)) {
            return std::unexpected(ProtocolError::InvalidFloat);
        }

        if (!isNormalized(target.bbox.x) ||
            !isNormalized(target.bbox.y) ||
            !isNormalized(target.bbox.width) ||
            !isNormalized(target.bbox.height)) {
            return std::unexpected(ProtocolError::InvalidBoundingBox);
        }
    }

    return {};
}

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;

    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (kCrc32cPolynomial & mask);
        }
    }

    return ~crc;
}

std::vector<std::uint8_t> encodeFrameMetadata(const FrameMetadata& metadata) {
    if (const auto validation = validate(metadata); !validation) {
        throw std::invalid_argument(
            std::string("Invalid frame metadata: ") +
            protocolErrorString(validation.error()));
    }

    const std::size_t packetSize =
        kWireHeaderSize +
        metadata.targets.size() * kWireTargetSize +
        kWireCrcSize;

    if (packetSize > kMaxFrameMetadataDatagramSize) {
        throw std::invalid_argument("Frame metadata exceeds the V1 UDP datagram limit");
    }

    std::vector<std::uint8_t> out;
    out.reserve(packetSize);

    out.insert(out.end(), kMagic.begin(), kMagic.end());
    appendU16(out, kProtocolVersion);
    appendU16(out, static_cast<std::uint16_t>(PacketType::FrameMetadata));
    appendU32(out, static_cast<std::uint32_t>(packetSize));
    appendU32(out, metadata.sequence);
    appendU64(out, metadata.key.streamEpoch);
    appendU64(out, metadata.key.frameId);
    appendU64(out, metadata.cvBeginNs);
    appendU64(out, metadata.cvEndNs);
    appendU16(out, static_cast<std::uint16_t>(metadata.targets.size()));
    appendU16(out, metadata.flags);

    for (const auto& target : metadata.targets) {
        appendU64(out, target.id);
        appendU16(out, target.classId);
        appendU16(out, target.flags);
        appendFloat(out, target.confidence);
        appendFloat(out, target.bbox.x);
        appendFloat(out, target.bbox.y);
        appendFloat(out, target.bbox.width);
        appendFloat(out, target.bbox.height);
    }

    appendU32(out, crc32c(out));

    if (out.size() != packetSize) {
        throw std::logic_error("Metadata encoder produced an unexpected packet size");
    }

    return out;
}

std::expected<FrameMetadata, ProtocolError> decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt) {
    if (datagram.size() < kWireHeaderSize + kWireCrcSize) {
        return std::unexpected(ProtocolError::PacketTooSmall);
    }
    if (datagram.size() > kMaxFrameMetadataDatagramSize) {
        return std::unexpected(ProtocolError::PacketTooLarge);
    }

    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (datagram[i] != kMagic[i]) {
            return std::unexpected(ProtocolError::BadMagic);
        }
    }

    if (readU16(datagram, 4) != kProtocolVersion) {
        return std::unexpected(ProtocolError::UnsupportedVersion);
    }
    if (readU16(datagram, 6) !=
        static_cast<std::uint16_t>(PacketType::FrameMetadata)) {
        return std::unexpected(ProtocolError::UnsupportedType);
    }

    const std::uint32_t declaredSize = readU32(datagram, 8);
    if (declaredSize != datagram.size()) {
        return std::unexpected(ProtocolError::SizeMismatch);
    }

    const std::uint16_t objectCount = readU16(datagram, 48);
    if (objectCount > kMaxTargetsPerFrame) {
        return std::unexpected(ProtocolError::InvalidTargetCount);
    }

    const std::size_t expectedSize =
        kWireHeaderSize +
        static_cast<std::size_t>(objectCount) * kWireTargetSize +
        kWireCrcSize;
    if (datagram.size() != expectedSize) {
        return std::unexpected(ProtocolError::SizeMismatch);
    }

    const std::size_t crcOffset = datagram.size() - kWireCrcSize;
    const std::uint32_t expectedCrc = readU32(datagram, crcOffset);
    if (crc32c(datagram.first(crcOffset)) != expectedCrc) {
        return std::unexpected(ProtocolError::CrcMismatch);
    }

    FrameMetadata metadata{
        .key = media::FrameKey{
            .streamEpoch = readU64(datagram, 16),
            .frameId = readU64(datagram, 24),
        },
        .sequence = readU32(datagram, 12),
        .cvBeginNs = readU64(datagram, 32),
        .cvEndNs = readU64(datagram, 40),
        .flags = readU16(datagram, 50),
        .targets = {},
        .receivedAt = receivedAt,
    };
    metadata.targets.reserve(objectCount);

    std::size_t offset = kWireHeaderSize;
    for (std::uint16_t index = 0; index < objectCount; ++index) {
        TargetMetadata target{
            .id = readU64(datagram, offset),
            .classId = readU16(datagram, offset + 8),
            .flags = readU16(datagram, offset + 10),
            .confidence = readFloat(datagram, offset + 12),
            .bbox = NormalizedBox{
                .x = readFloat(datagram, offset + 16),
                .y = readFloat(datagram, offset + 20),
                .width = readFloat(datagram, offset + 24),
                .height = readFloat(datagram, offset + 28),
            },
        };
        metadata.targets.push_back(target);
        offset += kWireTargetSize;
    }

    if (const auto validation = validate(metadata); !validation) {
        return std::unexpected(validation.error());
    }

    return metadata;
}

const char* protocolErrorString(ProtocolError error) noexcept {
    switch (error) {
    case ProtocolError::PacketTooSmall:
        return "packet too small";
    case ProtocolError::PacketTooLarge:
        return "packet too large";
    case ProtocolError::BadMagic:
        return "bad magic";
    case ProtocolError::UnsupportedVersion:
        return "unsupported protocol version";
    case ProtocolError::UnsupportedType:
        return "unsupported packet type";
    case ProtocolError::SizeMismatch:
        return "packet size mismatch";
    case ProtocolError::InvalidTargetCount:
        return "invalid target count";
    case ProtocolError::InvalidTiming:
        return "invalid CV timing";
    case ProtocolError::InvalidFloat:
        return "non-finite floating-point value";
    case ProtocolError::InvalidConfidence:
        return "confidence outside [0, 1]";
    case ProtocolError::InvalidBoundingBox:
        return "bounding-box coordinate outside [0, 1]";
    case ProtocolError::CrcMismatch:
        return "CRC32C mismatch";
    }

    return "unknown protocol error";
}

} // namespace reg::metadata
