#include "metadata/MetadataProtocol.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace wire = reg::metadata::wire;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

reg::metadata::FrameMetadata sampleMetadata() {
    reg::metadata::FrameMetadata metadata;
    metadata.key = {.streamEpoch = 7, .frameId = 123456};
    metadata.packetSequence = 991;
    metadata.cvBeginNs = 1'000'000;
    metadata.cvEndNs = 1'012'345;

    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 42,
        .classId = 3,
        .flags = 5,
        .confidence = 0.875F,
        .bbox = {
            .x = 0.25F,
            .y = 0.125F,
            .width = 0.5F,
            .height = 0.75F,
        },
    });

    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 43,
        .classId = 9,
        .flags = 0,
        .confidence = 1.0F,
        .bbox = {
            .x = 0.0F,
            .y = 0.0F,
            .width = 0.1F,
            .height = 0.2F,
        },
    });

    return metadata;
}

void crcMatchesKnownCastagnoliVector() {
    constexpr std::string_view text = "123456789";
    const auto bytes = std::span(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size());

    require(wire::crc32c(bytes) == 0xE3069283U, "CRC32C known vector mismatch");
}

void roundTripPreservesFrameAndTargets() {
    const auto source = sampleMetadata();
    const auto packet = wire::encodeFrameMetadata(source);
    const auto receivedAt = std::chrono::steady_clock::time_point{} + 1234ms;

    require(packet.size() <= wire::kMaxDatagramSize, "encoded datagram exceeds MTU-oriented limit");

    const auto decoded = wire::decodeFrameMetadata(packet, receivedAt);
    require(static_cast<bool>(decoded), "valid metadata packet failed to decode");
    require(decoded.metadata->key == source.key, "FrameKey did not round-trip");
    require(decoded.metadata->packetSequence == source.packetSequence, "packet sequence mismatch");
    require(decoded.metadata->cvBeginNs == source.cvBeginNs, "cvBeginNs mismatch");
    require(decoded.metadata->cvEndNs == source.cvEndNs, "cvEndNs mismatch");
    require(decoded.metadata->receivedAt == receivedAt, "local receive timestamp mismatch");
    require(decoded.metadata->targets.size() == source.targets.size(), "target count mismatch");

    const auto& target = decoded.metadata->targets.at(0);
    require(target.id == 42, "target id mismatch");
    require(target.classId == 3, "target class mismatch");
    require(target.flags == 5, "target flags mismatch");
    require(target.confidence == 0.875F, "target confidence mismatch");
    require(target.bbox.x == 0.25F, "bbox x mismatch");
    require(target.bbox.y == 0.125F, "bbox y mismatch");
    require(target.bbox.width == 0.5F, "bbox width mismatch");
    require(target.bbox.height == 0.75F, "bbox height mismatch");
}

void corruptionIsRejectedByCrc() {
    auto packet = wire::encodeFrameMetadata(sampleMetadata());
    packet.at(wire::kHeaderSize + 3) ^= 0x80U;

    const auto result = wire::decodeFrameMetadata(packet, {});
    require(result.error == wire::DecodeError::BadCrc, "payload corruption did not fail CRC");
}

void badMagicAndVersionAreRejectedBeforePayloadUse() {
    auto badMagic = wire::encodeFrameMetadata(sampleMetadata());
    badMagic[0] = 'X';
    require(
        wire::decodeFrameMetadata(badMagic, {}).error == wire::DecodeError::BadMagic,
        "bad magic was accepted");

    auto badVersion = wire::encodeFrameMetadata(sampleMetadata());
    badVersion[4] = 2;
    badVersion[5] = 0;
    require(
        wire::decodeFrameMetadata(badVersion, {}).error ==
            wire::DecodeError::UnsupportedVersion,
        "unsupported version was accepted");
}

void truncatedAndOversizedPacketsAreRejected() {
    auto packet = wire::encodeFrameMetadata(sampleMetadata());
    packet.pop_back();

    require(
        wire::decodeFrameMetadata(packet, {}).error == wire::DecodeError::SizeMismatch,
        "truncated packet was not rejected");

    std::vector<std::uint8_t> oversized(wire::kMaxDatagramSize + 1, 0);
    require(
        wire::decodeFrameMetadata(oversized, {}).error == wire::DecodeError::TooLarge,
        "oversized packet was not rejected");
}

void invalidFloatingPointDataCannotBeEncoded() {
    auto metadata = sampleMetadata();
    metadata.targets[0].confidence = std::numeric_limits<float>::quiet_NaN();

    bool threw = false;
    try {
        (void)wire::encodeFrameMetadata(metadata);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "NaN target data was encoded");
}

void invalidBoundingBoxCannotBeEncoded() {
    auto metadata = sampleMetadata();
    metadata.targets[0].bbox.x = 0.9F;
    metadata.targets[0].bbox.width = 0.5F;

    bool threw = false;
    try {
        (void)wire::encodeFrameMetadata(metadata);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "out-of-range normalized bbox was encoded");
}

void targetCountIsBounded() {
    auto metadata = sampleMetadata();
    metadata.targets.assign(
        wire::kMaxTargets + 1,
        reg::metadata::TargetMetadata{
            .id = 1,
            .confidence = 0.5F,
            .bbox = {.x = 0.0F, .y = 0.0F, .width = 0.1F, .height = 0.1F},
        });

    bool threw = false;
    try {
        (void)wire::encodeFrameMetadata(metadata);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "target count above protocol maximum was encoded");
}

void invalidCvTimingCannotBeEncoded() {
    auto metadata = sampleMetadata();
    metadata.cvBeginNs = 200;
    metadata.cvEndNs = 100;

    bool threw = false;
    try {
        (void)wire::encodeFrameMetadata(metadata);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "negative CV duration was encoded");
}

} // namespace

int main() {
    try {
        crcMatchesKnownCastagnoliVector();
        roundTripPreservesFrameAndTargets();
        corruptionIsRejectedByCrc();
        badMagicAndVersionAreRejectedBeforePayloadUse();
        truncatedAndOversizedPacketsAreRejected();
        invalidFloatingPointDataCannotBeEncoded();
        invalidBoundingBoxCannotBeEncoded();
        targetCountIsBounded();
        invalidCvTimingCannotBeEncoded();

        std::cout << "metadata_protocol_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "metadata_protocol_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
