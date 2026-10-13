#include "diagnostics/ExactSyncDiagnostics.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono;
using reg::diagnostics::ExactSyncDiagnostics;
using reg::media::FrameKey;
using reg::metadata::FrameMetadata;

void require(bool ok) {
    if (!ok) throw std::runtime_error("Exact Sync lifecycle assertion failed");
}
FrameMetadata meta(FrameKey key, steady_clock::time_point time, unsigned targets = 0) {
    FrameMetadata value;
    value.key = key;
    value.receivedAt = time;
    value.targets.resize(targets);
    return value;
}

void testLegacyAndHealthy() {
    ExactSyncDiagnostics tracker;
    const auto base = steady_clock::time_point{seconds{10}};
    const FrameKey a{12, 1}, b{12, 2};
    tracker.videoDecoded(a);
    tracker.overlayBuffered(a, base, base + milliseconds{150});
    auto value = meta(a, base + milliseconds{50}, 2);
    tracker.metadataAccepted(value);
    require(tracker.exactPair(a, value, base, base + milliseconds{150}, base + milliseconds{150}));
    require(tracker.exactPair(a, value, base, base + milliseconds{150}, base + milliseconds{150}));
    tracker.videoDecoded(b);
    tracker.overlayBuffered(b, base + milliseconds{20}, base + milliseconds{170});
    value = meta(b, base + milliseconds{120});
    tracker.metadataAccepted(value);
    require(tracker.exactPair(
        b, value, base + milliseconds{20}, base + milliseconds{170}, base + milliseconds{180}));
    const auto snap = tracker.snapshot();
    require(snap.matchedPairs == 2 && snap.presentedExactTotal == 2 && snap.eligibleFramesTotal == 2);
    require(snap.validEmptyDetectionsTotal == 1 && snap.metadataWithinDeadlineTotal == 2);
    require(snap.matchCoverage && *snap.matchCoverage == 1.0 && snap.latencySampleCount == 2);
    require(std::abs(snap.arrivalDelayP95Ms - 100.0) < .001);
    require(snap.lastMatchedTargetCount == 0 && snap.lastMatchedKey == b);
    require(snap.decidedFramesTotal == 2);
}

void testEarlyAndLateAndCensor() {
    ExactSyncDiagnostics tracker;
    const auto base = steady_clock::time_point{seconds{100}};
    const FrameKey early{2, 20}, afterDeadline{2, 21}, afterDrop{2, 22}, unobserved{2, 23};
    auto value = meta(early, base - milliseconds{5});
    tracker.metadataAccepted(value);
    tracker.videoDecoded(early);
    tracker.overlayBuffered(early, base, base + milliseconds{100});
    require(tracker.exactPair(early, value, base, base + milliseconds{100}, base + milliseconds{100}));
    require(tracker.snapshot().lastArrivalRelativeToDecodeMs == -5);
    tracker.overlayBuffered(afterDeadline, base, base + milliseconds{100});
    value = meta(afterDeadline, base + milliseconds{120});
    tracker.metadataAccepted(value);
    require(tracker.exactPair(
        afterDeadline, value, base, base + milliseconds{100}, base + milliseconds{130}));
    tracker.overlayBuffered(afterDrop, base, base + milliseconds{100});
    tracker.frameDueWithoutMetadata(afterDrop, base + milliseconds{100});
    tracker.frameDueWithoutMetadata(afterDrop, base + milliseconds{100});
    value = meta(afterDrop, base + milliseconds{140});
    tracker.metadataAccepted(value);
    tracker.metadataAccepted(value);
    tracker.overlayBuffered(unobserved, base, base + milliseconds{100});
    tracker.frameDueWithoutMetadata(unobserved, base + milliseconds{100});
    const auto snap = tracker.snapshot();
    require(snap.eligibleFramesTotal == 4 && snap.dropMissingTotal == 2);
    require(snap.metadataBeforeDecodeTotal == 1 && snap.metadataLateObservedTotal == 2);
    require(snap.metadataAfterDeadlineBeforeDecisionTotal == 1);
    require(snap.metadataAfterDroppedFrame == 1 && snap.metadataArrivedAfterDeadline == 1);
    require(snap.metadataDuplicateTotal == 1 && snap.matchedAfterDeadline == 1);
    require(snap.matchCoverage && std::abs(*snap.matchCoverage - .5) < .001);
    tracker.expire(base + seconds{6});
    const auto expired = tracker.snapshot();
    require(expired.metadataUnobservedAtExpiryTotal == 1);
    require(expired.retainedLifecycleCount == 0);
}

void testEpochIdentityAndEviction() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{200}};
    const FrameKey a{7, 42}, b{8, 42}, c{8, 41};
    tracker.videoDecoded(a);
    tracker.videoDecoded(a);
    tracker.videoDecoded(b);
    tracker.videoDecoded(c);
    const auto value = meta(b, now + milliseconds{1});
    tracker.metadataAccepted(value);
    tracker.overlayBuffered(a, now, now + milliseconds{100});
    tracker.overlayBuffered(b, now, now + milliseconds{100});
    tracker.overlayBuffered(b, now, now + milliseconds{100});
    require(!tracker.exactPair(a, value, now, now + milliseconds{100}, now + milliseconds{100}));
    tracker.frameBufferEvicted(a, now + milliseconds{100});
    tracker.frameBufferEvicted(a, now + milliseconds{100});
    require(tracker.exactPair(b, value, now, now + milliseconds{100}, now + milliseconds{100}));
    tracker.missingOverlayIdentity();
    const auto snap = tracker.snapshot();
    require(snap.videoEpochTransitionsTotal == 1 && snap.duplicateVideoKeysTotal == 1);
    require(snap.nonmonotonicVideoKeysTotal == 1 && snap.rejectedKeyMismatches == 1);
    require(snap.eligibleFramesTotal == 2 && snap.evictedTotal == 1);
    require(snap.duplicateLifecycleKeysTotal == 1 && snap.missingIdentityTotal == 1);
    require(snap.presentedExactTotal == 1 && snap.decidedFramesTotal == 2);
}

void testLegacyDropAndReset() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{500}};
    const FrameKey key{4, 10};
    tracker.frameDueWithoutMetadata(key, now);
    const auto value = meta(key, now + milliseconds{20});
    tracker.metadataAccepted(value);
    tracker.metadataAccepted(value);
    require(tracker.snapshot().metadataAfterDroppedFrame == 1);
    require(tracker.snapshot().metadataArrivedAfterDeadline == 1);
    tracker.clearPending();
    tracker.metadataAccepted(value);
    require(tracker.snapshot().metadataAfterDroppedFrame == 1);
    tracker.resetSession();
    const auto snap = tracker.snapshot();
    require(snap.sessionGeneration == 1 && snap.decidedFramesTotal == 0 &&
            snap.matchedPairs == 0 && snap.latencySampleCount == 0 && !snap.matchCoverage);
}

void testBoundedAndUnknown() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{700}};
    for (std::uint64_t i = 0; i < 400; ++i) {
        const FrameKey key{10, i};
        tracker.videoDecoded(key);
        tracker.overlayBuffered(key, now + milliseconds{static_cast<int>(i)},
            now + milliseconds{static_cast<int>(i) + 100});
    }
    auto snap = tracker.snapshot();
    require(snap.retainedLifecycleCount == ExactSyncDiagnostics::kHistoryCapacity);
    require(snap.capacityCensoredTotal == 144 && snap.historyForcedRetirementTotal == 144 &&
            snap.pendingFrameCount == 256);
    tracker.expire(now + seconds{10});
    snap = tracker.snapshot();
    require(snap.unknownAtExpiryTotal == 256 && snap.pendingFrameCount == 0);
    for (std::uint64_t i = 0; i < 300; ++i) {
        const FrameKey key{12, i};
        const auto value = meta(key,
            now + milliseconds{static_cast<int>(i)} - milliseconds{5});
        tracker.exactPair(key, value, now + milliseconds{static_cast<int>(i)},
            now + seconds{1}, now + seconds{1});
    }
    snap = tracker.snapshot();
    require(snap.latencySampleCount == 256 && snap.matchedPairs == 300);
    require(snap.arrivalDelayP50Ms == 0 && snap.arrivalDelayP99Ms == 0);
    tracker.overlayBuffered({13, 1}, now, now + seconds{1});
    tracker.clearPending();
    require(tracker.snapshot().unknownAtClearTotal == 1);
}

void testReorderAndExpiryBoundary() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{800}};
    const FrameKey a{15, 1}, b{15, 2}, c{15, 3};
    // Receive in reverse order; never match neighboring timestamps.
    tracker.metadataAccepted(meta(c, now + milliseconds{30}));
    tracker.metadataAccepted(meta(b, now + milliseconds{20}));
    tracker.metadataAccepted(meta(a, now + milliseconds{10}));
    tracker.overlayBuffered(a, now, now + milliseconds{100});
    tracker.overlayBuffered(b, now, now + milliseconds{100});
    tracker.overlayBuffered(c, now, now + milliseconds{100});
    require(tracker.exactPair(a, meta(a, now + milliseconds{10}),
        now, now + milliseconds{100}, now + milliseconds{100}));
    require(tracker.exactPair(b, meta(b, now + milliseconds{20}),
        now, now + milliseconds{100}, now + milliseconds{100}));
    require(tracker.exactPair(c, meta(c, now + milliseconds{30}),
        now, now + milliseconds{100}, now + milliseconds{100}));
    require(tracker.snapshot().presentedExactTotal == 3);
    const FrameKey missing{15, 4};
    tracker.overlayBuffered(missing, now, now + milliseconds{100});
    tracker.frameDueWithoutMetadata(missing, now + milliseconds{100});
    tracker.expire(now + milliseconds{100} +
        ExactSyncDiagnostics::kRetentionAfterDeadline - nanoseconds{1});
    require(tracker.snapshot().metadataUnobservedAtExpiryTotal == 0);
    tracker.expire(now + milliseconds{100} +
        ExactSyncDiagnostics::kRetentionAfterDeadline);
    require(tracker.snapshot().metadataUnobservedAtExpiryTotal == 1);
    // Ignore uncalibrated Jetson monotonic times for all Reg-local durations.
    auto value = meta({16, 1}, now + milliseconds{40});
    value.cvBeginNs = 999999999999ULL;
    value.cvEndNs = 9999999999999ULL;
    tracker.overlayBuffered(value.key, now, now + milliseconds{100});
    require(tracker.exactPair(
        value.key, value, now, now + milliseconds{100}, now + milliseconds{100}));
    require(std::abs(tracker.snapshot().lastArrivalRelativeToDecodeMs - 40.0) < .001);
}


void testSustainedMissingCvAt60Fps() {
    ExactSyncDiagnostics tracker;
    constexpr std::uint64_t frameCount = 600;
    const auto start = steady_clock::time_point{seconds{1200}};
    const auto frameTime = [&](std::uint64_t i) {
        return start + nanoseconds{
            static_cast<std::int64_t>((i * 1'000'000'000ULL) / 60ULL)};
    };

    // 10 seconds at 60 FPS, 150 ms of Overlay buffering, no CV metadata.
    // Drive expiry at EACH frame. The 256-key ring is smaller than the ~300
    // records necessary to retain a full 5-second follow-up interval.
    for (std::uint64_t i = 0; i < frameCount; ++i) {
        const FrameKey key{60, i};
        const auto decodedAt = frameTime(i);
        const auto deadline = decodedAt + milliseconds{150};
        tracker.videoDecoded(key);
        tracker.overlayBuffered(key, decodedAt, deadline);
        if (i >= 9) {
            tracker.frameDueWithoutMetadata({60, i - 9},
                frameTime(i - 9) + milliseconds{150});
        }
        tracker.expire(decodedAt); // periodic, virtual Reg steady-clock time
    }
    // Flush only normal due decisions, not diagnostic state.
    for (std::uint64_t i = frameCount - 9; i < frameCount; ++i) {
        tracker.frameDueWithoutMetadata({60, i}, frameTime(i) + milliseconds{150});
    }
    const auto lastDeadline = frameTime(frameCount - 1) + milliseconds{150};
    tracker.expire(lastDeadline);

    auto snap = tracker.snapshot();
    require(snap.eligibleFramesTotal == frameCount &&
            snap.dropMissingTotal == frameCount &&
            snap.decidedFramesTotal == frameCount);
    require(snap.matchCoverage && *snap.matchCoverage == 0);
    require(snap.metadataObservedTotal == 0 && snap.presentedExactTotal == 0);
    require(snap.metadataUnobservedAtExpiryTotal == 0);
    // Without the regression fix the 344 early retirements would be invisible
    // in metadataUnobservedAtExpiryTotal, making CV coverage look complete.
    require(snap.metadataUnobservedAtCapacityTotal == frameCount -
            ExactSyncDiagnostics::kHistoryCapacity);
    require(snap.historyForcedRetirementTotal ==
            snap.metadataUnobservedAtCapacityTotal);
    require(snap.decidedAwaitingMetadataCount ==
            ExactSyncDiagnostics::kHistoryCapacity);
    require(!snap.metadataObservationWindowComplete);

    tracker.expire(lastDeadline + ExactSyncDiagnostics::kRetentionAfterDeadline);
    snap = tracker.snapshot();
    require(snap.metadataUnobservedAtExpiryTotal ==
            ExactSyncDiagnostics::kHistoryCapacity);
    require(snap.metadataUnobservedAtCapacityTotal +
            snap.metadataUnobservedAtExpiryTotal == frameCount);
    require(snap.decidedAwaitingMetadataCount == 0 && snap.retainedLifecycleCount == 0);
    // Even after expiry, early capacity retirements have NOT enjoyed 5 s of
    // metadata follow-up: never equate them with proven delivery loss.
    require(!snap.metadataObservationWindowComplete);
    require(snap.metadataUnobservedAtClearTotal == 0);
    tracker.resetSession();
    snap = tracker.snapshot();
    require(snap.sessionGeneration == 1 &&
            snap.metadataUnobservedAtCapacityTotal == 0 &&
            snap.metadataObservationWindowComplete);
}

void testFlushCensorsDroppedMetadataFollowup() {
    ExactSyncDiagnostics tracker;
    const auto start = steady_clock::time_point{seconds{1400}};
    const FrameKey key{61, 1};
    tracker.overlayBuffered(key, start, start + milliseconds{150});
    tracker.frameDueWithoutMetadata(key, start + milliseconds{150});
    auto snap = tracker.snapshot();
    require(snap.decidedAwaitingMetadataCount == 1);
    require(!snap.metadataObservationWindowComplete);
    tracker.clearPending(); // e.g. decoder reconnect before metadata follow-up
    snap = tracker.snapshot();
    require(snap.dropMissingTotal == 1 &&
            snap.metadataUnobservedAtClearTotal == 1 &&
            snap.metadataUnobservedAtExpiryTotal == 0 &&
            snap.decidedAwaitingMetadataCount == 0 &&
            !snap.metadataObservationWindowComplete);
}

void testExactSelectionIsNotVulkanPresentation() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{1500}};
    const FrameKey key{62, 1};
    const auto value = meta(key, now + milliseconds{40});
    tracker.overlayBuffered(key, now, now + milliseconds{150});
    tracker.metadataAccepted(value);
    require(tracker.exactPair(
        key, value, now, now + milliseconds{150}, now + milliseconds{150}));
    // The frame is selected for presentation, BEFORE any renderer call.
    require(tracker.snapshot().presentedExactTotal == 1);
    const bool mockedVulkanRenderSucceeded = false; // no actual presentation
    require(!mockedVulkanRenderSucceeded);
    // A failed/skipped Vulkan render cannot be observed by this diagnostic
    // class: the counter measures selection, not a swapchain completion.
    require(tracker.snapshot().presentedExactTotal == 1);
    require(tracker.snapshot().matchedPairs == 1);
}

void testConcurrency() {
    ExactSyncDiagnostics tracker;
    const auto now = steady_clock::time_point{seconds{1000}};
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 1000; ++i) {
            const FrameKey key{99, i};
            tracker.videoDecoded(key);
            tracker.overlayBuffered(key, now + milliseconds{static_cast<int>(i)},
                now + seconds{1});
            const auto value = meta(key, now + milliseconds{static_cast<int>(i) + 2});
            tracker.metadataAccepted(value);
            require(tracker.exactPair(key, value,
                now + milliseconds{static_cast<int>(i)}, now + seconds{1}, now + seconds{1}));
        }
    });
    std::thread observer([&] {
        for (int i = 0; i < 1000; ++i) {
            const auto snap = tracker.snapshot();
            require(snap.retainedLifecycleCount <= ExactSyncDiagnostics::kHistoryCapacity);
            require(snap.decidedFramesTotal <= snap.eligibleFramesTotal);
        }
    });
    producer.join();
    observer.join();
    require(tracker.snapshot().matchedPairs == 1000);
}
} // namespace

int main() {
    testLegacyAndHealthy();
    testEarlyAndLateAndCensor();
    testEpochIdentityAndEviction();
    testLegacyDropAndReset();
    testBoundedAndUnknown();
    testReorderAndExpiryBoundary();
    testSustainedMissingCvAt60Fps();
    testFlushCensorsDroppedMetadataFollowup();
    testExactSelectionIsNotVulkanPresentation();
    testConcurrency();
}
