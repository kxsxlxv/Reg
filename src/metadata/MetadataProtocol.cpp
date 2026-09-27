#include "metadata/MetadataProtocol.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace reg::metadata::wire {
namespace {

constexpr std::uint8_t kMagic[4]{'C', 'V', 'M', '1'};
constexpr float kCoordinateTolerance = 0.001F;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendF32(std::vector<std::uint8_t>& out, float value) {
    appendU32(out, std::bit_cast<std::uint32_t>(value));
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
    }
    return value;
}

float readF32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<float>(readU32(bytes, offset));
}

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool validNormalizedCoordinate(float value) noexcept {
    return finite(value) &&
           value >= -kCoordinateTolerance &&
           value <= 1.0F + kCoordinateTolerance;
}

DecodeError validateTarget(const TargetMetadata& target) noexcept {
    if (!finite(target.confidence) ||
        !finite(target.bbox.x) ||
        !finite(target.bbox.y) ||
        !finite(target.bbox.width) ||
        !finite(target.bbox.height)) {
        return DecodeError::NonFiniteNumber;
    }

    if (target.confidence < 0.0F || target.confidence > 1.0F) {
        return DecodeError::InvalidConfidence;
    }

    if (!validNormalizedCoordinate(target.bbox.x) ||
        !validNormalizedCoordinate(target.bbox.y) ||
        !validNormalizedCoordinate(target.bbox.width) ||
        !validNormalizedCoordinate(target.bbox.height) ||
        target.bbox.width < 0.0F ||
        target.bbox.height < 0.0F ||
        target.bbox.x + target.bbox.width > 1.0F + kCoordinateTolerance ||
        target.bbox.y + target.bbox.height > 1.0F + kCoordinateTolerance) {
        return DecodeError::InvalidBoundingBox;
    }

    return DecodeError::None;
}

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept {
    // Reflected Castagnoli polynomial. Metadata traffic is tiny (~60 packets/s),
    // so the compact bitwise implementation is preferable to a global table.
    constexpr std::uint32_t polynomial = 0x82F63B78U;

    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (polynomial & mask);
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> encodeFrameMetadata(const FrameMetadata& metadata) {
    if (metadata.targets.size() > kMaxTargets) {
        throw std::invalid_argument("Frame metadata exceeds protocol target-count limit");
    }
    if (metadata.cvBeginNs != 0 &&
        metadata.cvEndNs != 0 &&
        metadata.cvEndNs < metadata.cvBeginNs) {
        throw std::invalid_argument("Frame metadata has invalid CV timing");
    }

    for (const auto& target : metadata.targets) {
        const DecodeError error = validateTarget(target);
        if (error != DecodeError::None) {
            throw std::invalid_argument(
                std::string("Cannot encode target: ") + decodeErrorName(error));
        }
    }

    const std::size_t packetSize =
        kHeaderSize + metadata.targets.size() * kTargetSize + kCrcSize;
    if (packetSize > kMaxDatagramSize ||
        packetSize > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Frame metadata exceeds maximum UDP datagram size");
    }

    std::vector<std::uint8_t> out;
    out.reserve(packetSize);

    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    appendU16(out, kProtocolVersion);
    appendU16(out, static_cast<std::uint16_t>(MessageType::FrameMetadata));
    appendU32(out, static_cast<std::uint32_t>(packetSize));
    appendU32(out, metadata.packetSequence);
    appendU64(out, metadata.key.streamEpoch);
    appendU64(out, metadata.key.frameId);
    appendU64(out, metadata.cvBeginNs);
    appendU64(out, metadata.cvEndNs);
    appendU16(out, static_cast<std::uint16_t>(metadata.targets.size()));
    appendU16(out, 0); // flags reserved for v1

    for (const auto& target : metadata.targets) {
        appendU64(out, target.id);
        appendU16(out, target.classId);
        appendU16(out, target.flags);
        appendF32(out, target.confidence);
        appendF32(out, target.bbox.x);
        appendF32(out, target.bbox.y);
        appendF32(out, target.bbox.width);
        appendF32(out, target.bbox.height);
    }

    appendU32(out, crc32c(out));
    return out;
}

DecodeResult decodeFrameMetadata(
    std::span<const std::uint8_t> datagram,
    std::chrono::steady_clock::time_point receivedAt) {
    if (datagram.size() < kHeaderSize + kCrcSize) {
        return {.error = DecodeError::TooShort};
    }
    if (datagram.size() > kMaxDatagramSize) {
        return {.error = DecodeError::TooLarge};
    }

    if (!std::equal(std::begin(kMagic), std::end(kMagic), datagram.begin())) {
        return {.error = DecodeError::BadMagic};
    }

    const std::uint16_t version = readU16(datagram, 4);
    if (version != kProtocolVersion) {
        return {.error = DecodeError::UnsupportedVersion};
    }

    const auto messageType =
        static_cast<MessageType>(readU16(datagram, 6));
    if (messageType != MessageType::FrameMetadata) {
        return {.error = DecodeError::UnsupportedMessageType};
    }

    const std::uint32_t declaredSize = readU32(datagram, 8);
    if (declaredSize != datagram.size()) {
        return {.error = DecodeError::SizeMismatch};
    }

    const std::uint16_t targetCount = readU16(datagram, 48);
    if (targetCount > kMaxTargets) {
        return {.error = DecodeError::TooManyTargets};
    }

    const std::size_t expectedSize =
        kHeaderSize + static_cast<std::size_t>(targetCount) * kTargetSize + kCrcSize;
    if (datagram.size() != expectedSize) {
        return {.error = DecodeError::SizeMismatch};
    }

    const std::uint32_t expectedCrc =
        readU32(datagram, datagram.size() - kCrcSize);
    const std::uint32_t actualCrc =
        crc32c(datagram.first(datagram.size() - kCrcSize));
    if (expectedCrc != actualCrc) {
        return {.error = DecodeError::BadCrc};
    }

    auto metadata = std::make_shared<FrameMetadata>();
    metadata->packetSequence = readU32(datagram, 12);
    metadata->key.streamEpoch = readU64(datagram, 16);
    metadata->key.frameId = readU64(datagram, 24);
    metadata->cvBeginNs = readU64(datagram, 32);
    metadata->cvEndNs = readU64(datagram, 40);
    metadata->receivedAt = receivedAt;

    if (metadata->cvBeginNs != 0 &&
        metadata->cvEndNs != 0 &&
        metadata->cvEndNs < metadata->cvBeginNs) {
        return {.error = DecodeError::InvalidTiming};
    }

    metadata->targets.reserve(targetCount);

    std::size_t offset = kHeaderSize;
    for (std::uint16_t index = 0; index < targetCount; ++index) {
        TargetMetadata target{};
        target.id = readU64(datagram, offset);
        target.classId = readU16(datagram, offset + 8);
        target.flags = readU16(datagram, offset + 10);
        target.confidence = readF32(datagram, offset + 12);
        target.bbox.x = readF32(datagram, offset + 16);
        target.bbox.y = readF32(datagram, offset + 20);
        target.bbox.width = readF32(datagram, offset + 24);
        target.bbox.height = readF32(datagram, offset + 28);

        const DecodeError targetError = validateTarget(target);
        if (targetError != DecodeError::None) {
            return {.error = targetError};
        }

        metadata->targets.push_back(target);
        offset += kTargetSize;
    }

    return {
        .metadata = std::move(metadata),
        .error = DecodeError::None,
    };
}

const char* decodeErrorName(DecodeError error) noexcept {
    switch (error) {
    case DecodeError::None:
        return "none";
    case DecodeError::TooShort:
        return "too_short";
    case DecodeError::TooLarge:
        return "too_large";
    case DecodeError::BadMagic:
        return "bad_magic";
    case DecodeError::UnsupportedVersion:
        return "unsupported_version";
    case DecodeError::UnsupportedMessageType:
        return "unsupported_message_type";
    case DecodeError::SizeMismatch:
        return "size_mismatch";
    case DecodeError::TooManyTargets:
        return "too_many_targets";
    case DecodeError::BadCrc:
        return "bad_crc";
    case DecodeError::NonFiniteNumber:
        return "non_finite_number";
    case DecodeError::InvalidConfidence:
        return "invalid_confidence";
    case DecodeError::InvalidBoundingBox:
        return "invalid_bounding_box";
    case DecodeError::InvalidTiming:
        return "invalid_timing";
    }
    return "unknown";
}

} // namespace reg::metadata::wire
