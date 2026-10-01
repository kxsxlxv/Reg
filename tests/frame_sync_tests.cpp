#include "metadata/MetadataStore.hpp"
#include "metadata/PacketSequenceTracker.hpp"
#include "metadata/Protocol.hpp"
#include "video/FrameSynchronizer.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

reg::metadata::FrameMetadata makeMetadata(
    std::uint64_t epoch,
    std::uint64_t frameId,
    std::uint32_t sequence) {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {.streamEpoch = epoch, .frameId = frameId};
    metadata.sequence = sequence;
    metadata.cvBeginNs = 1000;
    metadata.cvEndNs = 2000;
    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 42,
        .classId = 7,
        .flags = 3,
        .confidence = 0.875F,
        .bbox = {
            .x = 0.1F,
            .y = 0.2F,
            .width = 0.3F,
            .height = 0.4F,
        },
    });
    return metadata;
}

void crc32cMatchesKnownVector() {
    constexpr std::array<std::uint8_t, 9> bytes{
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };

    require(
        reg::metadata::protocol::crc32c(bytes) == 0xe3069283U,
        "CRC-32C known vector mismatch");
}

void protocolRoundTripPreservesFrameIdentity() {
    const auto source = makeMetadata(11, 9001, 1234);
    const auto packet = reg::metadata::protocol::encodeFrameMetadata(source);

    require(
        packet.size() <= reg::metadata::protocol::kMaxDatagramSize,
        "encoded packet exceeds datagram policy");

    const auto receivedAt = std::chrono::steady_clock::time_point{123ms};
    const auto decoded =
        reg::metadata::protocol::decodeFrameMetadata(packet, receivedAt);

    require(static_cast<bool>(decoded), "valid packet failed to decode");
    require(decoded.metadata.key == source.key, "FrameKey round-trip mismatch");
    require(decoded.metadata.sequence == source.sequence, "sequence mismatch");
    require(decoded.metadata.cvBeginNs == source.cvBeginNs, "cvBeginNs mismatch");
    require(decoded.metadata.cvEndNs == source.cvEndNs, "cvEndNs mismatch");
    require(decoded.metadata.targets.size() == 1, "target count mismatch");
    require(decoded.metadata.targets[0].id == 42, "target ID mismatch");
    require(decoded.metadata.receivedAt == receivedAt, "receive timestamp mismatch");
}

void corruptedPacketIsRejected() {
    auto packet =
        reg::metadata::protocol::encodeFrameMetadata(makeMetadata(1, 2, 3));
    packet[20] ^= 0x80U;

    const auto decoded = reg::metadata::protocol::decodeFrameMetadata(packet);
    require(
        decoded.error == reg::metadata::protocol::DecodeError::CrcMismatch,
        "corrupted packet was not rejected by CRC");
}

void invalidNormalizedRectIsRejected() {
    auto metadata = makeMetadata(1, 2, 3);
    metadata.targets[0].bbox.x = -0.1F;

    bool threw = false;
    try {
        static_cast<void>(
            reg::metadata::protocol::encodeFrameMetadata(metadata));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "invalid normalized bbox must fail encoding");
}

void metadataStoreUpgradesExactKeyAndStaysBounded() {
    reg::metadata::MetadataStore store(2);

    require(
        store.insert(makeMetadata(1, 10, 10)) ==
            reg::metadata::InsertResult::Inserted,
        "first metadata insert failed");

    auto upgraded = makeMetadata(1, 10, 11);
    upgraded.targets[0].flags = 0x0001U;
    require(
        store.insert(std::move(upgraded)) ==
            reg::metadata::InsertResult::Replaced,
        "newer snapshot for exact FrameKey did not replace pending value");

    const auto current = store.find({1, 10});
    require(current != nullptr, "upgraded exact-key metadata disappeared");
    require(current->sequence == 11U, "exact-key upgrade kept old sequence");
    require(
        current->targets[0].flags == 0x0001U,
        "exact-key upgrade kept old target provenance");

    require(
        store.insert(makeMetadata(1, 10, 10)) ==
            reg::metadata::InsertResult::Duplicate,
        "older exact-key packet was allowed to replace newer snapshot");

    require(
        store.insert(makeMetadata(1, 11, 12)) ==
            reg::metadata::InsertResult::Inserted,
        "second unique metadata insert failed");
    require(
        store.insert(makeMetadata(1, 12, 13)) ==
            reg::metadata::InsertResult::EvictedOldest,
        "bounded store did not evict oldest entry");

    require(!store.find({1, 10}), "oldest metadata was not evicted");
    require(store.find({1, 11}) != nullptr, "metadata 11 unexpectedly missing");
    require(store.find({1, 12}) != nullptr, "metadata 12 unexpectedly missing");
    require(store.size() == 2, "MetadataStore exceeded capacity");
}

void overlayBufferHonorsDeadlineAndCapacity() {
    const auto t0 = std::chrono::steady_clock::time_point{1s};
    reg::video::OverlayFrameBuffer frames(100ms, 2);

    require(
        frames.push({1, 100}, {}, t0) ==
            reg::video::OverlayPushResult::Inserted,
        "first frame insert failed");
    require(
        !frames.takeDue(t0 + 99ms).has_value(),
        "frame became due before deadline");

    auto due = frames.takeDue(t0 + 100ms);
    require(due.has_value(), "frame was not due at exact deadline");
    require(due->key == reg::media::FrameKey{1, 100}, "wrong frame became due");

    frames.push({1, 101}, {}, t0 + 1ms);
    frames.push({1, 102}, {}, t0 + 2ms);
    require(
        frames.push({1, 103}, {}, t0 + 3ms) ==
            reg::video::OverlayPushResult::EvictedOldest,
        "overflow did not evict oldest video frame");

    const auto front = frames.front();
    require(front.has_value(), "buffer unexpectedly empty");
    require(front->key == reg::media::FrameKey{1, 102}, "wrong frame survived eviction");
    require(frames.size() == 2, "OverlayFrameBuffer exceeded capacity");
}

void synchronizerPresentsOnlyExactFrameKey() {
    const auto t0 = std::chrono::steady_clock::time_point{5s};

    reg::video::OverlayFrameBuffer frames(100ms, 8);
    reg::metadata::MetadataStore metadata(8);
    reg::video::FrameSynchronizer synchronizer(frames, metadata);

    frames.push({7, 500}, {}, t0);
    metadata.insert(makeMetadata(7, 501, 1));

    require(
        synchronizer.next(t0 + 99ms).type ==
            reg::video::SyncDecisionType::None,
        "synchronizer made a decision before playout deadline");

    const auto mismatchDecision = synchronizer.next(t0 + 100ms);
    require(
        mismatchDecision.type ==
            reg::video::SyncDecisionType::DropMissingMetadata,
        "mismatched metadata must not be presented");
    require(
        mismatchDecision.droppedKey.has_value() &&
            *mismatchDecision.droppedKey == reg::media::FrameKey{7, 500},
        "wrong frame reported as dropped");
    require(
        metadata.find({7, 501}) != nullptr,
        "unrelated metadata must remain untouched");

    frames.push({7, 502}, {}, t0 + 200ms);
    metadata.insert(makeMetadata(7, 502, 2));

    const auto exactDecision = synchronizer.next(t0 + 300ms);
    require(
        exactDecision.type == reg::video::SyncDecisionType::Present,
        "exact FrameKey pair was not presented");
    require(exactDecision.frame.has_value(), "present decision has no frame");
    require(
        exactDecision.frame->buffered.key ==
            exactDecision.frame->metadata->key,
        "presented video and metadata FrameKeys differ");
    require(
        exactDecision.frame->metadata->key ==
            reg::media::FrameKey{7, 502},
        "unexpected exact pair was presented");
}

void packetSequenceTrackerHandlesLossReorderAndWrap() {
    reg::metadata::PacketSequenceTracker tracker;

    auto observation = tracker.observe(100);
    require(
        !observation.duplicate &&
            !observation.outOfOrder &&
            observation.missingBefore == 0,
        "first packet sequence observation is incorrect");

    observation = tracker.observe(103);
    require(
        observation.missingBefore == 2 &&
            !observation.outOfOrder,
        "forward sequence gap was not detected");

    observation = tracker.observe(103);
    require(observation.duplicate, "duplicate sequence was not detected");

    observation = tracker.observe(102);
    require(observation.outOfOrder, "late sequence was not detected");

    tracker.reset();
    tracker.observe(0xfffffffeU);
    observation = tracker.observe(1U);
    require(
        !observation.outOfOrder &&
            observation.missingBefore == 2,
        "uint32 sequence wrap-around was not handled as forward progress");
}

void changingDelayRecomputesBufferedDeadlines() {
    const auto t0 = std::chrono::steady_clock::time_point{9s};
    reg::video::OverlayFrameBuffer frames(150ms, 4);
    frames.push({3, 1}, {}, t0);

    frames.setPlayoutDelay(50ms);

    require(
        !frames.takeDue(t0 + 49ms).has_value(),
        "adjusted frame became due too early");
    require(
        frames.takeDue(t0 + 50ms).has_value(),
        "adjusted frame deadline was not recomputed");
}

} // namespace

int main() {
    try {
        crc32cMatchesKnownVector();
        protocolRoundTripPreservesFrameIdentity();
        corruptedPacketIsRejected();
        invalidNormalizedRectIsRejected();
        metadataStoreUpgradesExactKeyAndStaysBounded();
        overlayBufferHonorsDeadlineAndCapacity();
        synchronizerPresentsOnlyExactFrameKey();
        packetSequenceTrackerHandlesLossReorderAndWrap();
        changingDelayRecomputesBufferedDeadlines();

        std::cout << "frame_sync_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "frame_sync_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
