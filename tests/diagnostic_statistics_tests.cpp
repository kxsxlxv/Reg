#include "diagnostics/stats/Statistics.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace reg::diagnostics::stats;

namespace {

void require(bool ok, std::string_view msg) {
    if (!ok) throw std::runtime_error(std::string(msg));
}
void near(double a, double b, double tol, std::string_view msg) {
    require(std::abs(a - b) <= tol, msg);
}
MetricIdentity metric(MetricKind kind, std::string session = "test-1",
                      std::string population = "matched_only", std::uint64_t generation = 1) {
    return {"reg.test", "ns", "windows-reg", std::move(population), "reg-steady",
            std::move(session), generation, kind};
}

void testHistogramPrecisionAndMerge() {
    require(histogramUpperBound(histogramBucket(0)) == 0, "zero histogram");
    for (auto v : {1ULL, 15ULL, 16ULL, 31ULL, 999ULL, 1000000ULL,
                   180000000ULL, static_cast<unsigned long long>(UINT64_MAX)}) {
        const auto upper = histogramUpperBound(histogramBucket(v));
        require(upper >= v, "histogram upper must not underestimate");
        if (v >= 16 && v < UINT64_MAX / 2)
            require(static_cast<long double>(upper) - v < static_cast<long double>(v) / 16 + 2,
                    "histogram relative error");
    }
    DurationWindow one(metric(MetricKind::Duration), 32);
    DurationWindow two(metric(MetricKind::Duration), 32);
    for (int i = 0; i < 5; ++i) {
        require(one.recordDuration(i, static_cast<std::uint64_t>(10 + i)), "insert one");
        require(two.recordDuration(i, static_cast<std::uint64_t>(20 + i)), "insert two");
    }
    const auto a = one.snapshotBetween(0, 4, 4);
    const auto b = two.snapshotBetween(0, 4, 4);
    require(!a.mergedWith(b), "unverified same-window shards cannot merge");
    auto merged = a.mergedWith(b, Disjointness::VerifiedDisjoint);
    require(merged.has_value(), "attested disjoint same-window histogram shards merge");
    require(merged->sampleCount == 10 && merged->minNs == 10 && merged->maxNs == 24,
            "merged distribution extrema/count");
    require(merged->sumNs == 170 && merged->meanNs() == 17, "merged mean is weighted");
    require(!merged->rejectedInputs, "lifetime invalid-input counts must not be summed");
    require(merged->percentilesAreUpperBoundEstimates &&
            merged->quantileRelativeUpperErrorBound == 0.0625,
            "percentile accuracy metadata must survive merge");
    require(merged->percentileUpper(50).has_value(), "merged quantile exists");
    auto incompatible = b;
    incompatible.metric.population = "all_eligible";
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "population merge rejection");
    incompatible = b;
    incompatible.metric.sessionId = "restart";
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "session merge rejection");
    incompatible = b;
    incompatible.metric.resetGeneration = 2;
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "reset generation merge rejection");
    incompatible = b;
    incompatible.metric.clockDomain = "jetson-steady";
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "cross-host clock merge rejection");
    incompatible = b;
    incompatible.windowEndNs = 3;
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "window merge rejection");
    incompatible = b;
    incompatible.histogram[histogramBucket(10)] = UINT64_MAX;
    require(!a.mergedWith(incompatible, Disjointness::VerifiedDisjoint), "histogram overflow must reject merge");
}

void testKnownLatenciesAndOutcomes() {
    DurationWindow series(metric(MetricKind::Duration), 32);
    for (std::uint64_t i = 1; i <= 10; ++i)
        require(series.recordDuration(static_cast<std::int64_t>(i), i), "latency write");
    require(series.recordOutcome(11, Outcome::Timeout), "timeout write");
    require(series.recordOutcome(12, Outcome::Missing), "missing write");
    require(series.recordOutcome(13, Outcome::Late), "late write");
    require(series.recordOutcome(14, Outcome::Skipped), "skip write");
    const auto s = series.snapshotBetween(0, 14, 14);
    require(s.sampleCount == 10 && s.sumNs == 55 && s.meanNs() == 5.5,
            "sample count, sum, mean");
    require(s.minNs == 1 && s.maxNs == 10, "min max exact");
    require(s.percentileUpper(50) == 5 && s.percentileUpper(95) == 10 &&
            s.percentileUpper(99) == 10 && s.percentileUpper(0) == 1,
            "nearest-rank exact sub-16ns quantiles");
    require(s.observedCount() == 14 && s.coverage() == 10.0 / 14,
            "failures count toward denominator but not successful duration samples");
    require(s.outcomes[static_cast<unsigned>(Outcome::Timeout)] == 1, "timeout class");
    require(s.outcomes[static_cast<unsigned>(Outcome::Late)] == 1, "late class");
    require(!s.percentileUpper(101) && !s.percentileUpper(-1) &&
            !s.percentileUpper(std::numeric_limits<double>::quiet_NaN()), "bad percentile");
    require(!series.recordOutcome(15, Outcome::Success), "no outcome success without duration");
    require(!series.recordSeconds(15, std::numeric_limits<double>::infinity()), "infinity rejected");
    require(!series.recordSeconds(15, std::numeric_limits<double>::quiet_NaN()), "nan rejected");
    require(!series.recordSeconds(15, -0.1), "negative seconds rejected");
    require(!series.recordSeconds(15, 1e300), "overflow seconds rejected");
    require(!series.recordDuration(10, 20), "nonmonotonic rejected");
    require(series.snapshotBetween(0, 14, 14).rejectedInputs == 6, "invalid input count");
    require(series.recordSeconds(16, .001), "valid seconds");
    require(series.snapshotBetween(16, 16, 16).maxNs == 1'000'000ULL, "seconds conversion");
}

void testFramePacing() {
    for (const int hz : {60, 120}) {
        FramePacingWindow pacing(metric(MetricKind::FrameInterval, "source", "decoded"), 512);
        for (int i = 0; i <= hz; ++i) {
            const auto ns = static_cast<std::int64_t>(i) * kOneSecondNs / hz;
            require(pacing.recordFrame(ns), "constant rate ingest");
        }
        const auto s = pacing.snapshot(kOneSecondNs, kOneSecondNs);
        require(s.frameCount == static_cast<std::uint64_t>(hz + 1) &&
                s.intervals.sampleCount == static_cast<std::uint64_t>(hz),
                "exact number of within-window intervals");
        near(*s.fps, hz, 1e-6, "actual elapsed-time 60/120 fps");
        const auto gap = *s.longestGapNs();
        require(gap <= static_cast<std::uint64_t>((kOneSecondNs + hz - 1) / hz),
                "regular frame-gap upper bound");
        require(s.intervals.percentileUpper(95).has_value(), "p95 frame gap");
        const auto shortWindow = pacing.snapshotBetween(kOneSecondNs / 2,
                                                        kOneSecondNs, kOneSecondNs);
        near(*shortWindow.fps, hz, 1e-5, "window-local frame rate");
        require(shortWindow.intervals.sampleCount < s.intervals.sampleCount,
                "window bounds filter frames");
    }
    // Sixty intervals over exactly one second: 53 short, 6 long, one 180ms.
    // Mean FPS remains 60, but both P95 and P99 reveal degraded pacing.
    FramePacingWindow steady(metric(MetricKind::FrameInterval), 256);
    FramePacingWindow stall(metric(MetricKind::FrameInterval), 256);
    for (int i = 0; i <= 60; ++i)
        require(steady.recordFrame(static_cast<std::int64_t>(i) * kOneSecondNs / 60),
                "steady frame");
    require(stall.recordFrame(0), "stall start");
    for (int i = 1; i <= 53; ++i)
        require(stall.recordFrame(static_cast<std::int64_t>(i) * 640'000'000 / 53),
                "short-interval burst");
    for (int i = 1; i <= 6; ++i)
        require(stall.recordFrame(640'000'000 + static_cast<std::int64_t>(i) * 30'000'000),
                "long inter-frame interval");
    require(stall.recordFrame(kOneSecondNs), "180ms gap");
    const auto a = steady.snapshot(kOneSecondNs, kOneSecondNs);
    const auto b = stall.snapshot(kOneSecondNs, kOneSecondNs);
    near(*a.fps, *b.fps, 1e-9, "equal mean fps");
    require(*b.longestGapNs() >= 180'000'000ULL, "stall is visible in max gap");
    require(*b.intervals.percentileUpper(95) > *a.intervals.percentileUpper(95),
            "bursts visible in p95");
    require(*b.intervals.percentileUpper(99) > *a.intervals.percentileUpper(99),
            "stall is visible in p99");
    require(b.intervals.minNs < a.intervals.minNs, "burstiness visible in minimum");
    require(!stall.recordFrame(kOneSecondNs), "duplicate frame time rejected");
}

void testWindowsAndFreshness() {
    DurationWindow d(metric(MetricKind::Duration), 4, 100);
    auto s = d.snapshot(1000, 500);
    require(s.availability == Availability::Unavailable && s.sampleCount == 0 &&
            !s.meanNs() && !s.percentileUpper(95) && !s.coverage(), "startup unknown != 0");
    require(d.recordDuration(1000, 9), "first sample");
    require(d.recordDuration(1010, 10), "second sample");
    require(d.recordDuration(1020, 11), "third sample");
    require(d.recordDuration(1030, 12), "fourth sample");
    require(d.recordDuration(1040, 13), "fifth sample");
    s = d.snapshot(1040, 40);
    require(s.sampleCount == 4 && s.minNs == 10, "capacity evicts oldest, bounded ring");
    s = d.snapshot(1200, 30);
    require(s.sampleCount == 0 && s.availability == Availability::Unavailable &&
            s.sourceAvailability == Availability::Stale && !s.coverage(),
            "empty historical window unavailable, source stale, no fabricated healthy zero");
    s = d.snapshotBetween(1010, 1020, 1200);
    require(s.sampleCount == 2 && s.availability == Availability::Stale,
            "custom experiment boundaries inclusive");
    bool threw = false;
    try { (void)d.snapshot(100, -1); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "reject negative window");
}

void testCounters() {
    CounterRateWindow c(metric(MetricKind::Counter, "counter-session", "decoded_frames", 7), 64, 2 * kOneSecondNs);
    auto r = c.snapshot(kOneSecondNs, kOneSecondNs);
    require(!r.ratePerSecond && r.availability == Availability::Unavailable, "counter startup");
    require(c.observe(0, 2'000, 7), "first observation");
    r = c.snapshot(0, kOneSecondNs);
    require(!r.ratePerSecond, "first sample has no rate");
    require(c.observe(kOneSecondNs, 2'060, 7), "second observation");
    r = c.snapshot(kOneSecondNs, kOneSecondNs);
    require(r.pairCount == 1 && r.delta == 60 && r.ratePerSecond, "counter delta/rate");
    near(*r.ratePerSecond, 60, 1e-9, "60/s counter");
    require(!c.observe(kOneSecondNs, 2'061, 7), "zero-time sample is rejected");
    require(c.observe(2 * kOneSecondNs, 10, 8), "explicit reset generation");
    r = c.snapshot(2 * kOneSecondNs, kOneSecondNs);
    require(r.metric.resetGeneration == 8 && r.resetEvents == 1 && !r.ratePerSecond,
            "reset boundary cannot leak previous counter delta");
    require(c.observe(3 * kOneSecondNs, 20, 8), "post reset sample");
    require(c.observe(4 * kOneSecondNs, 5, 8), "implicit reset by decrease");
    r = c.snapshot(4 * kOneSecondNs, 4 * kOneSecondNs);
    require(r.resetEvents == 2 && r.observations == 1 && !r.ratePerSecond,
            "unmarked decrease is reset, not unsigned wrap");
    CounterRateWindow wrapped(metric(MetricKind::Counter));
    require(wrapped.observe(0, UINT64_MAX - 3, 1), "wrap start");
    require(wrapped.observe(kOneSecondNs, 5, 1, true), "owner-declared wrap");
    r = wrapped.snapshot(kOneSecondNs, kOneSecondNs);
    near(*r.ratePerSecond, 9, 1e-9, "64-bit explicitly declared wrap delta");
    require(!wrapped.observe(2 * kOneSecondNs, 6, 1, true), "invalid wrap marker");
    require(wrapped.observe(2 * kOneSecondNs, 6, 1), "still accepts non-wrapped sample");
    r = wrapped.snapshot(5 * kOneSecondNs, kOneSecondNs);
    require(r.availability == Availability::Unavailable &&
            r.sourceAvailability == Availability::Stale && !r.ratePerSecond,
            "empty historical counter window unavailable, source stale, rate not zero");
}

void testOverflowAndCapacity() {
    DurationWindow d(metric(MetricKind::Duration), 64);
    require(d.recordDuration(0, UINT64_MAX), "huge duration accepted");
    const auto s = d.snapshot(0, kOneSecondNs);
    require(s.maxNs == UINT64_MAX && s.percentileUpper(99) == UINT64_MAX,
            "largest uint64 duration safe");
}

void testHardResetUnsupportedAndStress() {
    auto unsupportedIdentity = metric(MetricKind::Duration);
    unsupportedIdentity.supported = false;
    DurationWindow unavailable(std::move(unsupportedIdentity));
    require(!unavailable.recordDuration(0, 7), "unsupported metric must reject ingestion");
    auto snapshot = unavailable.snapshot(0, kOneSecondNs);
    require(snapshot.availability == Availability::Unsupported &&
            !snapshot.meanNs() && snapshot.sampleCount == 0,
            "unsupported metric cannot masquerade as zero");
    CounterRateWindow restarts(metric(MetricKind::Counter), 8);
    require(restarts.observe(1000, 300, 1), "first session counter");
    require(restarts.observe(0, 0, 2), "generation restart with reset clock allowed");
    require(restarts.snapshot(0, kOneSecondNs).metric.resetGeneration == 2,
            "identity reset domain follows new generation");
    DurationWindow errors(metric(MetricKind::Duration), 256);
    for (int i = 0; i < 100'000; ++i)
        require(errors.recordOutcome(i, i % 2 ? Outcome::Missing : Outcome::Timeout),
                "high-frequency error bursts must remain bounded");
    const auto e = errors.snapshotBetween(99'744, 99'999, 99'999);
    require(e.observedCount() == 256 && e.sampleCount == 0 &&
            e.outcomes[static_cast<unsigned>(Outcome::Timeout)] == 128 &&
            e.outcomes[static_cast<unsigned>(Outcome::Missing)] == 128,
            "error burst outcomes remain bounded and correctly categorized");
    require(errors.memoryBoundBytes() <= 256 * 24,
            "duration data structure has a fixed memory bound");
    require(!e.percentileUpper(99) && !e.meanNs() && e.coverage() == 0,
            "zero coverage with known failures is not an unknown denominator");
}


void testWindowEvictionAndPartialStatistics() {
    DurationWindow d(metric(MetricKind::Duration), 3, 100);
    for (int t = 1; t <= 4; ++t)
        require(d.recordDuration(t, static_cast<std::uint64_t>(t * 10)), "duration sample");
    const auto incomplete = d.snapshotBetween(1, 4, 4);
    require(incomplete.historyTruncated && !incomplete.windowComplete &&
            incomplete.firstEvictedNs == 1 && incomplete.lastEvictedNs == 1,
            "duration eviction is explicitly visible");
    require(incomplete.sampleCount == 3 && !incomplete.meanNs() &&
            !incomplete.percentileUpper(95) && !incomplete.coverage(),
            "incomplete duration aggregate must not silently advertise valid tail");
    require(incomplete.meanNs(true) == 30 && incomplete.percentileUpper(95, true).has_value(),
            "explicitly requested partial descriptive aggregate is available");
    const auto complete = d.snapshotBetween(2, 4, 4);
    require(complete.windowComplete && complete.sampleCount == 3 &&
            complete.meanNs() == 30, "later fully retained window is complete");
    const auto deleted = d.snapshotBetween(1, 1, 4);
    require(!deleted.windowComplete && deleted.sampleCount == 0,
            "fully evicted historical window cannot be reported as empty healthy observation");
    DurationWindow d2(metric(MetricKind::Duration), 3, 100);
    for (int t = 1; t <= 4; ++t)
        require(d2.recordDuration(t, static_cast<std::uint64_t>(100 + t)), "second shard");
    const auto shard = d2.snapshotBetween(1, 4, 4);
    require(!incomplete.mergedWith(shard, Disjointness::VerifiedDisjoint),
            "known partial windows must refuse ordinary histogram merging");
    auto partial = incomplete.mergedWith(shard, Disjointness::VerifiedDisjoint, true);
    require(partial && !partial->windowComplete && partial->sampleCount == 6 &&
            !partial->percentileUpper(95) && partial->percentileUpper(95, true),
            "opt-in partial merge must still carry incomplete provenance");

    FramePacingWindow frames(metric(MetricKind::FrameInterval), 3, 100);
    for (int t = 1; t <= 4; ++t)
        require(frames.recordFrame(t), "frame timestamp");
    const auto frameLoss = frames.snapshotBetween(1, 4, 4);
    require(!frameLoss.intervals.windowComplete && frameLoss.intervals.historyTruncated &&
            frameLoss.intervals.firstEvictedNs == 1 && !frameLoss.fps &&
            !frameLoss.intervals.percentileUpper(95),
            "frame pacing refuses apparently correct truncated fps or percentile");
    const auto validPacing = frames.snapshotBetween(2, 4, 4);
    require(validPacing.intervals.windowComplete && validPacing.fps &&
            validPacing.intervals.sampleCount == 2 && *validPacing.fps == 1e9,
            "frame pacing has valid rate when window is wholly retained");

    CounterRateWindow counter(metric(MetricKind::Counter), 3, 100);
    for (int t = 1; t <= 4; ++t)
        require(counter.observe(t, static_cast<std::uint64_t>(t * 7), 1),
                "counter observation");
    const auto counterLoss = counter.snapshotBetween(1, 4, 4);
    require(counterLoss.historyTruncated && !counterLoss.windowComplete &&
            counterLoss.firstEvictedNs == 1 && !counterLoss.ratePerSecond,
            "truncated counter rates are unavailable by default");
    const auto validRate = counter.snapshotBetween(2, 4, 4);
    require(validRate.windowComplete && validRate.ratePerSecond &&
            *validRate.ratePerSecond == 7e9, "nontruncated counter rate");

    // Reset must invalidate a query crossing the previous counter session.
    CounterRateWindow resets(metric(MetricKind::Counter), 4, 100);
    require(resets.observe(1, 10, 1), "first generation");
    require(resets.observe(2, 11, 1), "same generation");
    require(resets.observe(3, 0, 2), "generation change");
    require(resets.observe(4, 1, 2), "new generation");
    const auto across = resets.snapshotBetween(1, 4, 4);
    require(!across.windowComplete && !across.ratePerSecond &&
            across.metric.resetGeneration == 2, "cross-reset window is partial");
    const auto after = resets.snapshotBetween(3, 4, 4);
    require(after.windowComplete && after.ratePerSecond &&
            *after.ratePerSecond == 1e9, "post-reset window is valid");
}

void testFreshnessAndNegativeTimestamps() {
    DurationWindow durations(metric(MetricKind::Duration), 16, 50);
    require(!durations.recordDuration(-1, 1), "negative timestamp rejected on empty ring");
    require(!durations.recordOutcome(-2, Outcome::Missing), "negative outcome timestamp rejected");
    require(durations.recordDuration(10, 7), "historical sample");
    require(durations.recordDuration(990, 8), "fresh stream sample");
    const auto old = durations.snapshotBetween(0, 20, 1000);
    require(old.lastObservationNs == 10 && old.lastSourceObservationNs == 990 &&
            old.availability == Availability::Stale &&
            old.sourceAvailability == Availability::Fresh,
            "window and stream freshness must remain distinct");
    const auto empty = durations.snapshotBetween(20, 30, 1000);
    require(!empty.lastObservationNs && empty.lastSourceObservationNs == 990 &&
            empty.availability == Availability::Unavailable &&
            empty.sourceAvailability == Availability::Fresh,
            "empty historical window may not borrow freshness from a later sample");
    const auto latest = durations.snapshotBetween(900, 1000, 1000);
    require(latest.availability == Availability::Fresh && latest.sourceAvailability == Availability::Fresh,
            "fresh in-window sample");

    FramePacingWindow frames(metric(MetricKind::FrameInterval), 16, 50);
    require(!frames.recordFrame(-1), "negative first frame rejected");
    require(frames.recordFrame(10) && frames.recordFrame(990), "frame observations");
    const auto oldFrames = frames.snapshotBetween(0, 20, 1000);
    require(oldFrames.intervals.availability == Availability::Stale &&
            oldFrames.intervals.sourceAvailability == Availability::Fresh &&
            oldFrames.intervals.lastObservationNs == 10, "historical frame freshness");
    CounterRateWindow counters(metric(MetricKind::Counter), 16, 50);
    require(!counters.observe(-1, 7, 2), "negative first counter rejected");
    require(counters.observe(10, 10, 1) && counters.observe(990, 15, 1),
            "counter observations");
    const auto oldCounter = counters.snapshotBetween(0, 20, 1000);
    require(oldCounter.availability == Availability::Stale &&
            oldCounter.sourceAvailability == Availability::Fresh &&
            oldCounter.lastObservationNs == 10, "historical counter freshness");
}

void testExplicitAdjacentMerge() {
    DurationWindow first(metric(MetricKind::Duration), 32);
    DurationWindow later(metric(MetricKind::Duration), 32);
    require(first.recordDuration(2, 4) && first.recordDuration(5, 6), "first interval");
    require(later.recordDuration(12, 8) && later.recordDuration(16, 10), "later interval");
    const auto a = first.snapshotBetween(0, 9, 19);
    const auto b = later.snapshotBetween(10, 19, 19);
    require(!a.mergedWith(b, Disjointness::VerifiedDisjoint), "ordinary merge requires equal window bounds");
    const auto combined = a.mergedAdjacentWith(b);
    require(combined && combined->windowStartNs == 0 && combined->windowEndNs == 19 &&
            combined->sampleCount == 4 && combined->sumNs == 28 &&
            combined->minNs == 4 && combined->maxNs == 10 &&
            combined->percentileUpper(95) == 10 && !combined->rejectedInputs,
            "explicit adjacent merge recomputes distribution over union interval");
    require(!b.mergedAdjacentWith(a), "adjacent merge must be ordered");
    const auto overlap = later.snapshotBetween(9, 19, 19);
    const auto gap = later.snapshotBetween(11, 19, 19);
    require(!a.mergedAdjacentWith(overlap) && !a.mergedAdjacentWith(gap),
            "overlap and uncovered time gaps cannot be silently concatenated");
    const auto mismatchedTime = later.snapshotBetween(10, 19, 20);
    require(!a.mergedAdjacentWith(mismatchedTime),
            "merging snapshots queried at different current times is ambiguous");
    auto incompatible = b;
    incompatible.metric.population = "different";
    require(!a.mergedAdjacentWith(incompatible), "adjacent merge must reject different populations");
    auto unverifiedSame = a.mergedWith(a);
    require(!unverifiedSame, "same-shard copy must not be counted twice implicitly");
}

void testCaptureBenchmark() {
    const auto identity = metric(MetricKind::FrameInterval);
    FramePacingWindow enabled(identity, 8192);
    std::int64_t timestamp = 0;
    constexpr int kEvents = 120 * 120;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kEvents; ++i) {
        timestamp += kOneSecondNs / 120;
        require(enabled.recordFrame(timestamp), "benchmark frame accepted");
    }
    const auto captured = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> disabledCounter{0};
    for (int i = 0; i < kEvents; ++i) disabledCounter.fetch_add(1, std::memory_order_relaxed);
    const auto disabled = std::chrono::steady_clock::now();
    const auto snapshot = enabled.snapshot(timestamp, kSixtySecondsNs);
    require(snapshot.intervals.sampleCount > 7000 && enabled.memoryBoundBytes() <= 8192 * sizeof(std::int64_t),
            "bounded frame capture under 120 samples/s");
    require(disabledCounter.load() == kEvents, "counter-only benchmark baseline");
    const double captureNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(captured - start).count()) / kEvents;
    const double disabledNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(disabled - captured).count()) / kEvents;
    std::cout << "bench_frame_capture_ns_per_sample=" << captureNs
              << " bench_atomic_counter_ns_per_sample=" << disabledNs
              << " ring_bytes=" << enabled.memoryBoundBytes() << '\n';
}
} // namespace

int main() {
    try {
        testHistogramPrecisionAndMerge();
        testKnownLatenciesAndOutcomes();
        testFramePacing();
        testWindowsAndFreshness();
        testCounters();
        testOverflowAndCapacity();
        testHardResetUnsupportedAndStress();
        testWindowEvictionAndPartialStatistics();
        testFreshnessAndNegativeTimestamps();
        testExplicitAdjacentMerge();
        testCaptureBenchmark();
        std::cout << "diagnostic_statistics_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "diagnostic_statistics_tests: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
