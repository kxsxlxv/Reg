#include "diagnostics/ExactSyncDiagnostics.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace {

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Exact Sync diagnostics regression failed");
    }
}

void testExactKeysAndTiming() {
    using namespace std::chrono;
    reg::diagnostics::ExactSyncDiagnostics tracker;
    const auto start = steady_clock::time_point{seconds{100}};
    const reg::media::FrameKey videoKey{.streamEpoch = 12, .frameId = 56};
    const reg::media::FrameKey wrongEpoch{.streamEpoch = 13, .frameId = 56};
    reg::metadata::FrameMetadata metadata;
    metadata.key = wrongEpoch;
    metadata.receivedAt = start + milliseconds{15};

    tracker.videoDecoded(videoKey);
    tracker.metadataAccepted(metadata);
    require(!tracker.exactPair(
        videoKey, metadata, start,
        start + milliseconds{130},
        start + milliseconds{130}));
    require(tracker.snapshot().rejectedKeyMismatches == 1U);
    require(tracker.snapshot().matchedPairs == 0U);

    metadata.key = videoKey;
    metadata.receivedAt = start + milliseconds{40};
    metadata.targets.resize(2);
    tracker.metadataAccepted(metadata);
    require(tracker.exactPair(
        videoKey, metadata, start,
        start + milliseconds{130},
        start + milliseconds{135}));

    const auto result = tracker.snapshot();
    require(result.lastVideoKey == videoKey);
    require(result.lastMetadataKey == videoKey);
    require(result.lastMatchedKey == videoKey);
    require(result.lastMatchedTargetCount == 2U);
    require(result.matchedPairs == 1U);
    require(result.matchedAfterDeadline == 0U);
    require(result.hasTimingSample);
    require(std::abs(result.lastArrivalRelativeToDecodeMs - 40.0) < 0.001);
    require(std::abs(result.lastPlayoutWaitMs - 135.0) < 0.001);
    require(std::abs(result.arrivalDelayP95Ms - 40.0) < 0.001);
}

void testLateAndOutOfOrderMetadata() {
    using namespace std::chrono;
    reg::diagnostics::ExactSyncDiagnostics tracker;
    const auto start = steady_clock::time_point{seconds{200}};
    const reg::media::FrameKey lostKey{.streamEpoch = 8, .frameId = 70};
    const reg::media::FrameKey otherKey{.streamEpoch = 8, .frameId = 71};
    const auto deadline = start + milliseconds{130};

    tracker.frameDueWithoutMetadata(lostKey, deadline);
    reg::metadata::FrameMetadata unrelated;
    unrelated.key = otherKey;
    unrelated.receivedAt = deadline + milliseconds{50};
    tracker.metadataAccepted(unrelated);
    require(tracker.snapshot().metadataAfterDroppedFrame == 0U);

    reg::metadata::FrameMetadata late;
    late.key = lostKey;
    late.receivedAt = deadline + milliseconds{20};
    tracker.metadataAccepted(late);
    tracker.metadataAccepted(late);
    auto snapshot = tracker.snapshot();
    require(snapshot.dueWithoutMetadata == 1U);
    require(snapshot.metadataAfterDroppedFrame == 1U);
    require(snapshot.metadataArrivedAfterDeadline == 1U);

    // Valid packet after deadline can still be paired if the renderer has not
    // consumed the frame yet. Report it separately from after-drop arrivals.
    reg::metadata::FrameMetadata delayed;
    delayed.key = otherKey;
    delayed.receivedAt = deadline + milliseconds{10};
    require(tracker.exactPair(
        otherKey, delayed, start, deadline,
        deadline + milliseconds{15}));
    snapshot = tracker.snapshot();
    require(snapshot.matchedPairs == 1U);
    require(snapshot.matchedAfterDeadline == 1U);
    require(snapshot.metadataAfterDroppedFrame == 1U);
    require(snapshot.lastArrivalRelativeToDecodeMs > 139.9);

    tracker.frameDueWithoutMetadata(
        reg::media::FrameKey{.streamEpoch = 8, .frameId = 72},
        deadline);
    tracker.clearPending();
    reg::metadata::FrameMetadata cleared;
    cleared.key = reg::media::FrameKey{.streamEpoch = 8, .frameId = 72};
    cleared.receivedAt = deadline + milliseconds{100};
    tracker.metadataAccepted(cleared);
    require(tracker.snapshot().metadataAfterDroppedFrame == 1U);
}

void testBoundedSamplesAndEarlyMetadata() {
    using namespace std::chrono;
    reg::diagnostics::ExactSyncDiagnostics tracker;
    const auto start = steady_clock::time_point{seconds{300}};
    for (std::uint64_t i = 1; i <= 300; ++i) {
        const auto decodedAt = start + milliseconds{static_cast<int>(i)};
        const reg::media::FrameKey key{.streamEpoch = 20, .frameId = i};
        reg::metadata::FrameMetadata metadata;
        metadata.key = key;
        metadata.receivedAt = decodedAt - milliseconds{5};
        require(tracker.exactPair(
            key, metadata, decodedAt,
            decodedAt + milliseconds{130},
            decodedAt + milliseconds{135}));
    }
    const auto snapshot = tracker.snapshot();
    require(snapshot.matchedPairs == 300U);
    require(snapshot.latencySampleCount == 256U);
    require(snapshot.arrivalDelayP50Ms == 0.0);
    require(snapshot.arrivalDelayP95Ms == 0.0);
    require(snapshot.arrivalDelayP99Ms == 0.0);
    require(snapshot.lastArrivalRelativeToDecodeMs == -5.0);
}

} // namespace

int main() {
    testExactKeysAndTiming();
    testLateAndOutOfOrderMetadata();
    testBoundedSamplesAndEarlyMetadata();
}
