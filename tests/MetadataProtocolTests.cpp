#include "metadata/MetadataStore.hpp"
#include "metadata/Protocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

reg::metadata::FrameMetadata sampleMetadata(
    std::uint32_t sequence,
    std::uint64_t frameId,
    std::chrono::steady_clock::time_point receivedAt) {
    return reg::metadata::FrameMetadata{
        .key = reg::media::FrameKey{
            .streamEpoch = 17,
            .frameId = frameId,
        },
        .sequence = sequence,
        .cvBeginNs = 1'000'000,
        .cvEndNs = 1'012'500,
        .flags = 3,
        .targets = {
            reg::metadata::TargetMetadata{
                .id = 42,
                .classId = 5,
                .flags = 1,
                .confidence = 0.875F,
                .bbox = reg::metadata::NormalizedBox{
                    .x = 0.10F,
                    .y = 0.20F,
                    .width = 0.30F,
                    .height = 0.40F,
                },
            },
            reg::metadata::TargetMetadata{
                .id = 99,
                .classId = 7,
                .flags = 0,
                .confidence = 0.50F,
                .bbox = reg::metadata::NormalizedBox{
                    .x = 0.55F,
                    .y = 0.15F,
                    .width = 0.20F,
                    .height = 0.25F,
                },
            },
        },
        .receivedAt = receivedAt,
    };
}

void testCrc32cKnownVector() {
    constexpr std::array<std::uint8_t, 9> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };

    require(
        reg::metadata::crc32c(input) == 0xE3069283U,
        "CRC32C known vector failed");
}

void testProtocolRoundTrip() {
    const auto receivedAt = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds(12345)};
    const auto source = sampleMetadata(100, 200, receivedAt);

    const auto packet = reg::metadata::encodeFrameMetadata(source);
    require(
        packet.size() ==
            reg::metadata::kWireHeaderSize +
            source.targets.size() * reg::metadata::kWireTargetSize +
            reg::metadata::kWireCrcSize,
        "encoded packet size mismatch");

    const auto decoded =
        reg::metadata::decodeFrameMetadata(packet, receivedAt);
    require(decoded.has_value(), "metadata packet did not decode");

    const auto& value = *decoded;
    require(value.key == source.key, "FrameKey changed during round trip");
    require(value.sequence == source.sequence, "sequence changed during round trip");
    require(value.cvBeginNs == source.cvBeginNs, "cvBeginNs changed during round trip");
    require(value.cvEndNs == source.cvEndNs, "cvEndNs changed during round trip");
    require(value.flags == source.flags, "flags changed during round trip");
    require(value.targets.size() == source.targets.size(), "target count changed");

    for (std::size_t i = 0; i < source.targets.size(); ++i) {
        const auto& left = source.targets[i];
        const auto& right = value.targets[i];

        require(left.id == right.id, "target id changed");
        require(left.classId == right.classId, "target class changed");
        require(left.flags == right.flags, "target flags changed");
        require(left.confidence == right.confidence, "target confidence changed");
        require(left.bbox.x == right.bbox.x, "bbox x changed");
        require(left.bbox.y == right.bbox.y, "bbox y changed");
        require(left.bbox.width == right.bbox.width, "bbox width changed");
        require(left.bbox.height == right.bbox.height, "bbox height changed");
    }
}

void testCorruptionRejected() {
    auto packet = reg::metadata::encodeFrameMetadata(
        sampleMetadata(101, 201, std::chrono::steady_clock::now()));

    packet[20] ^= 0x40U;

    const auto decoded = reg::metadata::decodeFrameMetadata(packet);
    require(!decoded.has_value(), "corrupted packet unexpectedly decoded");
    require(
        decoded.error() == reg::metadata::ProtocolError::CrcMismatch,
        "corrupted packet failed for the wrong reason");
}

void testInvalidBoundingBoxRejected() {
    auto metadata =
        sampleMetadata(102, 202, std::chrono::steady_clock::now());
    metadata.targets.front().bbox.width = 1.5F;

    bool rejected = false;
    try {
        static_cast<void>(reg::metadata::encodeFrameMetadata(metadata));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    require(rejected, "out-of-range bbox was not rejected");
}

void testMetadataStoreSemantics() {
    using namespace std::chrono_literals;

    const auto t0 = std::chrono::steady_clock::time_point{10s};

    reg::metadata::MetadataStore store(2, 10s);

    require(
        store.insert(sampleMetadata(1, 1, t0)) ==
            reg::metadata::MetadataInsertResult::Inserted,
        "first metadata insert failed");

    require(
        store.insert(sampleMetadata(1, 1, t0 + 1ms)) ==
            reg::metadata::MetadataInsertResult::Duplicate,
        "duplicate metadata was not identified");

    require(
        store.insert(sampleMetadata(2, 1, t0 + 2ms)) ==
            reg::metadata::MetadataInsertResult::Conflict,
        "same FrameKey with different sequence was not identified as conflict");

    require(
        store.insert(sampleMetadata(3, 2, t0 + 3ms)) ==
            reg::metadata::MetadataInsertResult::Inserted,
        "second unique metadata insert failed");

    require(
        store.insert(sampleMetadata(4, 3, t0 + 4ms)) ==
            reg::metadata::MetadataInsertResult::EvictedOldestAndInserted,
        "bounded store did not evict oldest metadata");

    require(
        store.find(reg::media::FrameKey{17, 1}) == nullptr,
        "oldest metadata was not evicted");

    const auto taken = store.take(reg::media::FrameKey{17, 2});
    require(taken != nullptr, "take did not return exact metadata");
    require(store.find(reg::media::FrameKey{17, 2}) == nullptr, "take did not erase metadata");
}

} // namespace

int main() {
    try {
        testCrc32cKnownVector();
        testProtocolRoundTrip();
        testCorruptionRejected();
        testInvalidBoundingBoxRejected();
        testMetadataStoreSemantics();

        std::cout << "reg_core_tests: all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "reg_core_tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
