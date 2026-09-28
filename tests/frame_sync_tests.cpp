#include "metadata/MetadataStore.hpp"
#include "video/FrameSynchronizer.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

struct FakeFrame {
    std::uint64_t value{};
};

using Buffer = reg::video::OverlayFrameBuffer<FakeFrame>;
using Synchronizer = reg::video::FrameSynchronizer<FakeFrame>;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

reg::metadata::FrameMetadataPtr metadataFor(
    reg::media::FrameKey key,
    std::uint32_t sequence = 1) {
    auto metadata = std::make_shared<reg::metadata::FrameMetadata>();
    metadata->key = key;
    metadata->packetSequence = sequence;
    return metadata;
}

std::shared_ptr<const FakeFrame> frame(std::uint64_t value) {
    return std::make_shared<const FakeFrame>(FakeFrame{.value = value});
}

void waitsUntilDeadlineEvenWhenMetadataArrivesEarly() {
    Buffer video(150ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey key{.streamEpoch = 2, .frameId = 100};

    video.push(key, frame(100), t0);
    metadata.insert(metadataFor(key));

    const auto before = sync.next(t0 + 149ms);
    require(before.action == reg::video::SyncAction::Wait, "frame presented before playout deadline");

    const auto atDeadline = sync.next(t0 + 150ms);
    require(atDeadline.action == reg::video::SyncAction::Present, "exact pair not presented at deadline");
    require(atDeadline.key == key, "presented key mismatch");
    require(atDeadline.frame && atDeadline.frame->value == 100, "frame payload mismatch");
    require(atDeadline.metadata && atDeadline.metadata->key == key, "metadata key mismatch");
}

void missingMetadataDropsFrameAtDeadline() {
    Buffer video(150ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey key{.streamEpoch = 3, .frameId = 200};

    video.push(key, frame(200), t0);

    const auto result = sync.next(t0 + 150ms);
    require(result.action == reg::video::SyncAction::Drop, "missing metadata must drop frame");
    require(result.key == key, "dropped key mismatch");
    require(result.frame && result.frame->value == 200, "dropped frame payload lost");
    require(!result.metadata, "drop unexpectedly carried metadata");
}

void wrongFrameMetadataIsNeverReused() {
    Buffer video(100ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey videoKey{.streamEpoch = 1, .frameId = 10};
    const reg::media::FrameKey staleKey{.streamEpoch = 1, .frameId = 9};

    video.push(videoKey, frame(10), t0);
    metadata.insert(metadataFor(staleKey));

    const auto result = sync.next(t0 + 100ms);
    require(result.action == reg::video::SyncAction::Drop, "stale metadata was applied to a newer frame");
    require(metadata.find(staleKey) != nullptr, "unrelated metadata should remain independently addressable");
}

void streamEpochIsPartOfIdentity() {
    Buffer video(50ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey current{.streamEpoch = 8, .frameId = 42};
    const reg::media::FrameKey previousEpoch{.streamEpoch = 7, .frameId = 42};

    video.push(current, frame(42), t0);
    metadata.insert(metadataFor(previousEpoch));

    const auto result = sync.next(t0 + 50ms);
    require(result.action == reg::video::SyncAction::Drop, "metadata from previous stream epoch matched current video");
}

void outOfOrderMetadataStillMatchesExactFrame() {
    Buffer video(100ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey first{.streamEpoch = 1, .frameId = 1};
    const reg::media::FrameKey second{.streamEpoch = 1, .frameId = 2};

    video.push(first, frame(1), t0);
    video.push(second, frame(2), t0 + 16ms);

    metadata.insert(metadataFor(second, 2));
    metadata.insert(metadataFor(first, 1));

    const auto firstResult = sync.next(t0 + 100ms);
    require(firstResult.action == reg::video::SyncAction::Present, "first exact pair did not present");
    require(firstResult.key == first, "out-of-order metadata changed video order");

    const auto secondResult = sync.next(t0 + 116ms);
    require(secondResult.action == reg::video::SyncAction::Present, "second exact pair did not present");
    require(secondResult.key == second, "second exact key mismatch");
}

void duplicateMetadataIsIgnored() {
    reg::metadata::MetadataStore metadata(4);
    const reg::media::FrameKey key{.streamEpoch = 1, .frameId = 55};

    const auto first = metadataFor(key, 100);
    const auto duplicate = metadataFor(key, 101);

    require(
        metadata.insert(first) == reg::metadata::MetadataInsertResult::Inserted,
        "first metadata insert failed");
    require(
        metadata.insert(duplicate) == reg::metadata::MetadataInsertResult::Duplicate,
        "duplicate metadata was not detected");

    const auto retained = metadata.find(key);
    require(retained && retained->packetSequence == 100, "duplicate replaced first metadata unexpectedly");
}

void buffersRemainBounded() {
    Buffer video(100ms, 2);
    const auto t0 = Buffer::Clock::time_point{} + 1s;

    video.push({1, 1}, frame(1), t0);
    video.push({1, 2}, frame(2), t0 + 1ms);

    require(
        video.push({1, 3}, frame(3), t0 + 2ms) ==
            Buffer::PushResult::EvictedOldestAndInserted,
        "video buffer did not report bounded eviction");
    require(video.size() == 2, "video buffer exceeded capacity");
    const auto front = video.peekFront();
    require(front && front->key.frameId == 2, "video buffer evicted wrong frame");

    reg::metadata::MetadataStore metadata(2);
    metadata.insert(metadataFor({1, 1}, 1));
    metadata.insert(metadataFor({1, 2}, 2));
    require(
        metadata.insert(metadataFor({1, 3}, 3)) ==
            reg::metadata::MetadataInsertResult::EvictedOldestAndInserted,
        "metadata store did not report bounded eviction");
    require(metadata.size() == 2, "metadata store exceeded capacity");
    require(!metadata.find({1, 1}), "metadata store retained evicted oldest entry");
}

void lateMetadataBehindWatermarkIsRejected() {
    Buffer video(50ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey expired{.streamEpoch = 4, .frameId = 77};

    video.push(expired, frame(77), t0);
    const auto dropped = sync.next(t0 + 50ms);

    require(dropped.action == reg::video::SyncAction::Drop, "frame without metadata was not dropped");
    require(dropped.droppedFrames == 1, "drop count mismatch");

    require(
        metadata.insert(metadataFor(expired, 5)) ==
            reg::metadata::MetadataInsertResult::TooLate,
        "late metadata behind watermark was accepted");
    require(metadata.size() == 0, "late metadata polluted the store");
}

void synchronizerDrainsMultipleExpiredMisses() {
    Buffer video(50ms, 16);
    reg::metadata::MetadataStore metadata;
    Synchronizer sync(video, metadata);

    const auto t0 = Buffer::Clock::time_point{} + 1s;
    const reg::media::FrameKey first{.streamEpoch = 1, .frameId = 10};
    const reg::media::FrameKey second{.streamEpoch = 1, .frameId = 11};
    const reg::media::FrameKey third{.streamEpoch = 1, .frameId = 12};

    video.push(first, frame(10), t0);
    video.push(second, frame(11), t0 + 1ms);
    video.push(third, frame(12), t0 + 2ms);
    metadata.insert(metadataFor(third, 3));

    const auto result = sync.next(t0 + 60ms);

    require(result.action == reg::video::SyncAction::Present, "ready exact pair was not reached");
    require(result.key == third, "synchronizer presented the wrong frame after draining misses");
    require(result.droppedFrames == 2, "expired misses were not drained in one synchronization decision");
}

void changingDelayRetimesBufferedFrames() {
    Buffer video(150ms, 4);
    const auto t0 = Buffer::Clock::time_point{} + 1s;
    video.push({1, 1}, frame(1), t0);

    video.setPlayoutDelay(120ms);

    const auto front = video.peekFront();
    require(front.has_value(), "retimed frame disappeared");
    require(front->deadline == t0 + 120ms, "existing frame deadline was not retimed");
}

} // namespace

int main() {
    try {
        waitsUntilDeadlineEvenWhenMetadataArrivesEarly();
        missingMetadataDropsFrameAtDeadline();
        wrongFrameMetadataIsNeverReused();
        streamEpochIsPartOfIdentity();
        outOfOrderMetadataStillMatchesExactFrame();
        duplicateMetadataIsIgnored();
        buffersRemainBounded();
        lateMetadataBehindWatermarkIsRejected();
        synchronizerDrainsMultipleExpiredMisses();
        changingDelayRetimesBufferedFrames();

        std::cout << "frame_sync_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "frame_sync_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
