#include "metadata/MetadataProtocol.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using namespace std::chrono_literals;
namespace wire = reg::metadata::wire;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeU16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void writeU32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        const auto shift = static_cast<unsigned>((3U - i) * 8U);
        bytes[offset + i] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

void refreshCrc(std::vector<std::uint8_t>& packet) {
    const auto crc = wire::crc32c(
        std::span<const std::uint8_t>(packet.data(), packet.size() - wire::kCrcSize));
    writeU32(packet, packet.size() - wire::kCrcSize, crc);
}

reg::metadata::FrameMetadata sampleMetadata() {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {.streamEpoch = 0x0102030405060708ULL, .frameId = 424242};
    metadata.packetSequence = 9876;
    metadata.flags = 0x15;
    metadata.cvBeginNs = 10'000;
    metadata.cvEndNs = 12'345;

    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 111,
        .classId = 7,
        .flags = 3,
        .confidence = 0.875F,
        .bbox = {.x = 0.1F, .y = 0.2F, .width = 0.3F, .height = 0.4F},
    });
    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 222,
        .classId = 9,
        .flags = 1,
        .confidence = 0.5F,
        .bbox = {.x = 0.55F, .y = 0.6F, .width = 0.2F, .height = 0.25F},
    });

    return metadata;
}

void crcMatchesCastagnoliReferenceVector() {
    constexpr std::string_view input = "123456789";
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input.data()),
        input.size());

    require(
        wire::crc32c(bytes) == 0xE3069283U,
        "CRC32C implementation does not match Castagnoli reference vector");
}

void frameMetadataRoundTrips() {
    const auto source = sampleMetadata();
    const auto packet = wire::encodeFrameMetadataPacket(source);
    const auto receiveTime =
        std::chrono::steady_clock::time_point{} + 5s;

    require(packet.size() < wire::kMaxDatagramSize, "normal packet exceeded MTU-safe target");

    const auto decoded =
        wire::decodeFrameMetadataPacket(packet, receiveTime);

    require(static_cast<bool>(decoded), "valid metadata packet failed to decode");
    require(decoded.metadata->key == source.key, "FrameKey changed during round trip");
    require(decoded.metadata->packetSequence == source.packetSequence, "sequence changed");
    require(decoded.metadata->flags == source.flags, "packet flags changed");
    require(decoded.metadata->cvBeginNs == source.cvBeginNs, "cvBeginNs changed");
    require(decoded.metadata->cvEndNs == source.cvEndNs, "cvEndNs changed");
    require(decoded.metadata->receivedAt == receiveTime, "local receive timestamp was not attached");
    require(decoded.metadata->targets.size() == source.targets.size(), "target count changed");

    const auto& target = decoded.metadata->targets.at(0);
    require(target.id == 111, "target id changed");
    require(target.classId == 7, "target class changed");
    require(target.flags == 3, "target flags changed");
    require(target.confidence == 0.875F, "confidence changed");
    require(target.bbox.x == 0.1F, "bbox x changed");
    require(target.bbox.y == 0.2F, "bbox y changed");
    require(target.bbox.width == 0.3F, "bbox width changed");
    require(target.bbox.height == 0.4F, "bbox height changed");
}

void corruptionIsRejectedByCrc() {
    auto packet = wire::encodeFrameMetadataPacket(sampleMetadata());
    packet[20] ^= 0x40U;

    const auto result = wire::decodeFrameMetadataPacket(
        packet,
        std::chrono::steady_clock::time_point{});

    require(result.error == wire::DecodeError::CrcMismatch, "corrupted packet bypassed CRC32C");
    require(!result.metadata, "corrupted packet produced metadata");
}

void targetCountLimitIsCheckedBeforeAllocation() {
    auto packet = wire::encodeFrameMetadataPacket(sampleMetadata());
    writeU16(packet, 48, static_cast<std::uint16_t>(wire::kMaxTargets + 1));

    const auto result = wire::decodeFrameMetadataPacket(
        packet,
        std::chrono::steady_clock::time_point{});

    require(
        result.error == wire::DecodeError::TooManyTargets,
        "oversized target count was not rejected");
}

void nonFiniteTargetValuesAreRejected() {
    auto packet = wire::encodeFrameMetadataPacket(sampleMetadata());

    const std::uint32_t nanBits =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    writeU32(packet, wire::kHeaderSize + 12, nanBits);
    refreshCrc(packet);

    const auto result = wire::decodeFrameMetadataPacket(
        packet,
        std::chrono::steady_clock::time_point{});

    require(
        result.error == wire::DecodeError::InvalidNumericValue,
        "NaN target value was accepted");
}

void declaredPacketSizeMustMatchDatagram() {
    auto packet = wire::encodeFrameMetadataPacket(sampleMetadata());
    writeU32(packet, 8, static_cast<std::uint32_t>(packet.size() + 1));

    const auto result = wire::decodeFrameMetadataPacket(
        packet,
        std::chrono::steady_clock::time_point{});

    require(
        result.error == wire::DecodeError::SizeMismatch,
        "mismatched declared packet size was accepted");
}

void outboundNormalizedValuesAreValidated() {
    auto metadata = sampleMetadata();
    metadata.targets.front().bbox.x = -0.1F;

    bool threw = false;
    try {
        static_cast<void>(wire::encodeFrameMetadataPacket(metadata));
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    require(threw, "encoder accepted out-of-range normalized coordinate");
}

} // namespace

int main() {
    try {
        crcMatchesCastagnoliReferenceVector();
        frameMetadataRoundTrips();
        corruptionIsRejectedByCrc();
        targetCountLimitIsCheckedBeforeAllocation();
        nonFiniteTargetValuesAreRejected();
        declaredPacketSizeMustMatchDatagram();
        outboundNormalizedValuesAreValidated();

        std::cout << "metadata_protocol_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "metadata_protocol_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
