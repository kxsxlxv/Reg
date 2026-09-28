#include "metadata/Protocol.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace reg::metadata::protocol {
namespace {

constexpr std::uint8_t kMagic[4]{'C', 'V', 'M', '1'};

void writeU16(
    std::span<std::uint8_t> out,
    std::size_t offset,
    std::uint16_t value) noexcept {
    out[offset] =
        static_cast<std::uint8_t>(value & 0xffU);
    out[offset + 1] =
        static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void writeU32(
    std::span<std::uint8_t> out,
    std::size_t offset,
    std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) {
        out[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

void writeU64(
    std::span<std::uint8_t> out,
    std::size_t offset,
    std::uint64_t value) noexcept {
    for (unsigned i = 0; i < 8; ++i) {
        out[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

void writeF32(
    std::span<std::uint8_t> out,
    std::size_t offset,
    float value) noexcept {
    writeU32(
        out,
        offset,
        std::bit_cast<std::uint32_t>(value));
}

std::uint16_t readU16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t readU32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |=
            static_cast<std::uint32_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::uint64_t readU64(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

float readF32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    return std::bit_cast<float>(
        readU32(bytes, offset));
}

bool isFiniteRect(const NormalizedRect& rect) noexcept {
    return std::isfinite(rect.x) &&
           std::isfinite(rect.y) &&
           std::isfinite(rect.width) &&
           std::isfinite(rect.height);
}

bool isNormalizedRect(const NormalizedRect& rect) noexcept {
    constexpr float epsilon = 0.001F;
    if (!isFiniteRect(rect)) {
        return false;
    }

    return rect.x >= 0.0F &&
           rect.y >= 0.0F &&
           rect.width >= 0.0F &&
           rect.height >= 0.0F &&
           rect.x <= 1.0F &&
           rect.y <= 1.0F &&
           rect.width <= 1.0F &&
           rect.height <= 1.0F &&
           rect.x + rect.width <= 1.0F + epsilon &&
           rect.y + rect.height <= 1.0F + epsilon;
}

std::size_t validateForEncode(
    const FrameMetadata& metadata) {
    if (metadata.targets.size() > kMaxTargets) {
        throw std::invalid_argument(
            "metadata target count exceeds protocol maximum");
    }

    const std::size_t packetSize =
        kHeaderSize +
        metadata.targets.size() * kTargetSize +
        kCrcSize;

    if (packetSize > kMaxDatagramSize) {
        throw std::invalid_argument(
            "metadata packet exceeds configured UDP datagram limit");
    }

    for (const auto& target : metadata.targets) {
        if (!std::isfinite(target.confidence)) {
            throw std::invalid_argument(
                "target confidence is not finite");
        }
        if (target.confidence < 0.0F ||
            target.confidence > 1.0F) {
            throw std::invalid_argument(
                "target confidence is outside [0,1]");
        }
        if (!isNormalizedRect(target.bbox)) {
            throw std::invalid_argument(
                "target bbox is not a valid normalized rectangle");
        }
    }

    return packetSize;
}

} // namespace

std::uint32_t crc32c(
    std::span<const std::uint8_t> bytes) noexcept {
    // Reflected CRC-32C (Castagnoli), polynomial 0x1EDC6F41.
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc =
                (crc >> 1U) ^
                (0x82f63b78U & mask);
        }
    }
    return ~crc;
}

std::size_t encodedFrameMetadataSize(
    const FrameMetadata& metadata) {
    return validateForEncode(metadata);
}

std::size_t encodeFrameMetadataInto(
    const FrameMetadata& metadata,
    std::span<std::uint8_t> destination) {
    const std::size_t packetSize =
        validateForEncode(metadata);

    if (destination.size() < packetSize) {
        throw std::length_error(
            "metadata destination buffer is too small");
    }

    const auto out =
        destination.first(packetSize);

    std::copy(
        std::begin(kMagic),
        std::end(kMagic),
        out.begin());

    writeU16(out, 4, kVersion);
    writeU16(
        out,
        6,
        static_cast<std::uint16_t>(
            MessageType::FrameMetadata));
    writeU32(
        out,
        8,
        static_cast<std::uint32_t>(
            packetSize));
    writeU32(out, 12, metadata.sequence);
    writeU64(
        out,
        16,
        metadata.key.streamEpoch);
    writeU64(
        out,
        24,
        metadata.key.frameId);
    writeU64(
        out,
        32,
        metadata.cvBeginNs);
    writeU64(
        out,
        40,
        metadata.cvEndNs);
    writeU16(
        out,
        48,
        static_cast<std::uint16_t>(
            metadata.targets.size()));
    writeU16(out, 50, metadata.flags);

    std::size_t offset = kHeaderSize;

    for (const auto& target :
         metadata.targets) {
        writeU64(out, offset, target.id);
        writeU16(
            out,
            offset + 8,
            target.classId);
        writeU16(
            out,
            offset + 10,
            target.flags);
        writeF32(
            out,
            offset + 12,
            target.confidence);
        writeF32(
            out,
            offset + 16,
            target.bbox.x);
        writeF32(
            out,
            offset + 20,
            target.bbox.y);
        writeF32(
            out,
            offset + 24,
            target.bbox.width);
        writeF32(
            out,
            offset + 28,
            target.bbox.height);

        offset += kTargetSize;
    }

    const std::uint32_t checksum =
        crc32c(
            std::span<const std::uint8_t>{
                out.data(),
                packetSize - kCrcSize});

    writeU32(
        out,
        packetSize - kCrcSize,
        checksum);

    return packetSize;
}

std::vector<std::uint8_t> encodeFrameMetadata(
    const FrameMetadata& metadata) {
    std::vector<std::uint8_t> out(
        encodedFrameMetadataSize(metadata));

    static_cast<void>(
        encodeFrameMetadataInto(
            metadata,
            out));

    return out;
}

DecodeResult decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt) {
    DecodeResult result{};

    if (datagram.size() <
        kHeaderSize + kCrcSize) {
        result.error =
            DecodeError::TooSmall;
        return result;
    }

    if (!std::equal(
            std::begin(kMagic),
            std::end(kMagic),
            datagram.begin())) {
        result.error =
            DecodeError::BadMagic;
        return result;
    }

    if (readU16(datagram, 4) != kVersion) {
        result.error =
            DecodeError::UnsupportedVersion;
        return result;
    }

    const auto type =
        static_cast<MessageType>(
            readU16(datagram, 6));

    if (type !=
        MessageType::FrameMetadata) {
        result.error =
            DecodeError::UnsupportedType;
        return result;
    }

    const std::uint32_t declaredSize =
        readU32(datagram, 8);

    if (declaredSize != datagram.size() ||
        declaredSize > kMaxDatagramSize) {
        result.error =
            DecodeError::SizeMismatch;
        return result;
    }

    const std::uint16_t targetCount =
        readU16(datagram, 48);

    if (targetCount > kMaxTargets) {
        result.error =
            DecodeError::TargetCountTooLarge;
        return result;
    }

    const std::size_t expectedSize =
        kHeaderSize +
        static_cast<std::size_t>(
            targetCount) *
            kTargetSize +
        kCrcSize;

    if (expectedSize != datagram.size()) {
        result.error =
            DecodeError::SizeMismatch;
        return result;
    }

    const std::uint32_t expectedCrc =
        readU32(
            datagram,
            datagram.size() - kCrcSize);

    const std::uint32_t actualCrc =
        crc32c(
            datagram.first(
                datagram.size() - kCrcSize));

    if (expectedCrc != actualCrc) {
        result.error =
            DecodeError::CrcMismatch;
        return result;
    }

    FrameMetadata metadata{};
    metadata.sequence =
        readU32(datagram, 12);
    metadata.key.streamEpoch =
        readU64(datagram, 16);
    metadata.key.frameId =
        readU64(datagram, 24);
    metadata.cvBeginNs =
        readU64(datagram, 32);
    metadata.cvEndNs =
        readU64(datagram, 40);
    metadata.flags =
        readU16(datagram, 50);
    metadata.receivedAt = receivedAt;
    metadata.targets.reserve(targetCount);

    std::size_t offset = kHeaderSize;

    for (std::uint16_t index = 0;
         index < targetCount;
         ++index) {
        TargetMetadata target{};
        target.id =
            readU64(datagram, offset);
        target.classId =
            readU16(datagram, offset + 8);
        target.flags =
            readU16(datagram, offset + 10);
        target.confidence =
            readF32(datagram, offset + 12);
        target.bbox.x =
            readF32(datagram, offset + 16);
        target.bbox.y =
            readF32(datagram, offset + 20);
        target.bbox.width =
            readF32(datagram, offset + 24);
        target.bbox.height =
            readF32(datagram, offset + 28);

        if (!std::isfinite(
                target.confidence)) {
            result.error =
                DecodeError::InvalidNumber;
            return result;
        }

        if (target.confidence < 0.0F ||
            target.confidence > 1.0F) {
            result.error =
                DecodeError::InvalidNumber;
            return result;
        }

        if (!isNormalizedRect(
                target.bbox)) {
            result.error =
                DecodeError::InvalidNormalizedRect;
            return result;
        }

        metadata.targets.push_back(
            target);
        offset += kTargetSize;
    }

    result.metadata =
        std::move(metadata);
    return result;
}

const char* toString(
    DecodeError error) noexcept {
    switch (error) {
    case DecodeError::None:
        return "none";
    case DecodeError::TooSmall:
        return "too_small";
    case DecodeError::BadMagic:
        return "bad_magic";
    case DecodeError::UnsupportedVersion:
        return "unsupported_version";
    case DecodeError::UnsupportedType:
        return "unsupported_type";
    case DecodeError::SizeMismatch:
        return "size_mismatch";
    case DecodeError::TargetCountTooLarge:
        return "target_count_too_large";
    case DecodeError::CrcMismatch:
        return "crc_mismatch";
    case DecodeError::InvalidNumber:
        return "invalid_number";
    case DecodeError::InvalidNormalizedRect:
        return "invalid_normalized_rect";
    }

    return "unknown";
}

} // namespace reg::metadata::protocol
