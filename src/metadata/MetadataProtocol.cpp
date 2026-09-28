#include "metadata/MetadataProtocol.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace reg::metadata::wire {
namespace {

constexpr std::uint8_t kMagic[4]{'C', 'V', 'M', '1'};
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

void writeU16(std::span<std::uint8_t> out, std::size_t offset, std::uint16_t value) {
    out[offset + 0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void writeU32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        const auto shift = static_cast<unsigned>((3U - i) * 8U);
        out[offset + i] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

void writeU64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        const auto shift = static_cast<unsigned>((7U - i) * 8U);
        out[offset + i] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return
        (static_cast<std::uint16_t>(bytes[offset + 0]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1]);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<std::uint32_t>(bytes[offset + i]);
    }
    return value;
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

void writeFloat(std::span<std::uint8_t> out, std::size_t offset, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    writeU32(out, offset, std::bit_cast<std::uint32_t>(value));
}

float readFloat(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<float>(readU32(bytes, offset));
}

bool normalized(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

bool validTarget(const TargetMetadata& target) noexcept {
    return normalized(target.confidence) &&
           normalized(target.bbox.x) &&
           normalized(target.bbox.y) &&
           normalized(target.bbox.width) &&
           normalized(target.bbox.height);
}

std::size_t expectedPacketSize(std::uint16_t targetCount) {
    return kHeaderSize +
           static_cast<std::size_t>(targetCount) * kTargetSize +
           kCrcSize;
}

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;

    for (const std::uint8_t byte : bytes) {
        crc ^= byte;

        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (kCrc32cPolynomial & mask);
        }
    }

    return ~crc;
}

std::vector<std::uint8_t> encodeFrameMetadataPacket(
    const FrameMetadata& metadata) {
    if (metadata.targets.size() > kMaxTargets) {
        throw std::invalid_argument("Metadata packet exceeds target-count limit");
    }

    for (const auto& target : metadata.targets) {
        if (!validTarget(target)) {
            throw std::invalid_argument(
                "Metadata packet contains non-finite or non-normalized target values");
        }
    }

    const auto targetCount =
        static_cast<std::uint16_t>(metadata.targets.size());
    const std::size_t packetSize = expectedPacketSize(targetCount);

    if (packetSize > kMaxDatagramSize ||
        packetSize > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Metadata packet exceeds datagram size limit");
    }

    std::vector<std::uint8_t> packet(packetSize, 0);
    std::span<std::uint8_t> out(packet);

    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    writeU16(out, 4, kProtocolVersion);
    writeU16(out, 6, static_cast<std::uint16_t>(PacketType::FrameMetadata));
    writeU32(out, 8, static_cast<std::uint32_t>(packetSize));
    writeU32(out, 12, metadata.packetSequence);
    writeU64(out, 16, metadata.key.streamEpoch);
    writeU64(out, 24, metadata.key.frameId);
    writeU64(out, 32, metadata.cvBeginNs);
    writeU64(out, 40, metadata.cvEndNs);
    writeU16(out, 48, targetCount);
    writeU16(out, 50, metadata.flags);

    std::size_t offset = kHeaderSize;
    for (const auto& target : metadata.targets) {
        writeU64(out, offset + 0, target.id);
        writeU16(out, offset + 8, target.classId);
        writeU16(out, offset + 10, target.flags);
        writeFloat(out, offset + 12, target.confidence);
        writeFloat(out, offset + 16, target.bbox.x);
        writeFloat(out, offset + 20, target.bbox.y);
        writeFloat(out, offset + 24, target.bbox.width);
        writeFloat(out, offset + 28, target.bbox.height);
        offset += kTargetSize;
    }

    const std::uint32_t crc =
        crc32c(std::span<const std::uint8_t>(packet.data(), packet.size() - kCrcSize));
    writeU32(out, packet.size() - kCrcSize, crc);

    return packet;
}

DecodeResult decodeFrameMetadataPacket(
    std::span<const std::uint8_t> packet,
    std::chrono::steady_clock::time_point receivedAt) {
    if (packet.size() < kHeaderSize + kCrcSize) {
        return {.error = DecodeError::TooShort};
    }
    if (packet.size() > kMaxDatagramSize) {
        return {.error = DecodeError::TooLarge};
    }
    if (std::memcmp(packet.data(), kMagic, sizeof(kMagic)) != 0) {
        return {.error = DecodeError::BadMagic};
    }

    const std::uint16_t version = readU16(packet, 4);
    if (version != kProtocolVersion) {
        return {.error = DecodeError::UnsupportedVersion};
    }

    const auto packetType = static_cast<PacketType>(readU16(packet, 6));
    if (packetType != PacketType::FrameMetadata) {
        return {.error = DecodeError::UnsupportedType};
    }

    const std::uint32_t declaredSize = readU32(packet, 8);
    if (declaredSize != packet.size()) {
        return {.error = DecodeError::SizeMismatch};
    }

    const std::uint16_t targetCount = readU16(packet, 48);
    if (targetCount > kMaxTargets) {
        return {.error = DecodeError::TooManyTargets};
    }
    if (expectedPacketSize(targetCount) != packet.size()) {
        return {.error = DecodeError::SizeMismatch};
    }

    const std::uint32_t expectedCrc =
        readU32(packet, packet.size() - kCrcSize);
    const std::uint32_t actualCrc =
        crc32c(packet.first(packet.size() - kCrcSize));

    if (expectedCrc != actualCrc) {
        return {.error = DecodeError::CrcMismatch};
    }

    auto metadata = std::make_shared<FrameMetadata>();
    metadata->packetSequence = readU32(packet, 12);
    metadata->key.streamEpoch = readU64(packet, 16);
    metadata->key.frameId = readU64(packet, 24);
    metadata->cvBeginNs = readU64(packet, 32);
    metadata->cvEndNs = readU64(packet, 40);
    metadata->flags = readU16(packet, 50);
    metadata->receivedAt = receivedAt;
    metadata->targets.reserve(targetCount);

    std::size_t offset = kHeaderSize;
    for (std::uint16_t index = 0; index < targetCount; ++index) {
        TargetMetadata target{};
        target.id = readU64(packet, offset + 0);
        target.classId = readU16(packet, offset + 8);
        target.flags = readU16(packet, offset + 10);
        target.confidence = readFloat(packet, offset + 12);
        target.bbox.x = readFloat(packet, offset + 16);
        target.bbox.y = readFloat(packet, offset + 20);
        target.bbox.width = readFloat(packet, offset + 24);
        target.bbox.height = readFloat(packet, offset + 28);

        if (!validTarget(target)) {
            return {.error = DecodeError::InvalidNumericValue};
        }

        metadata->targets.push_back(target);
        offset += kTargetSize;
    }

    return {
        .metadata = std::move(metadata),
        .error = DecodeError::None,
    };
}

} // namespace reg::metadata::wire
