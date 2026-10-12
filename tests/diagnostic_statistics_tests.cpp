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
    auto merged = a.mergedWith(b);
    require(merged.has_value(), "compatible histograms must merge");
    require(merged->sampleCount == 10 && merged->minNs == 10 && merged->maxNs == 24,
            "merged distribution extrema/count");
    require(merged->sumNs == 170 && merged->meanNs() == 17, "merged mean is weighted");
    require(merged->percentileUpper(50).has_value(), "merged quantile exists");
    auto incompatible = b;
    incompatible.metric.population = "all_eligible";
    require(!a.mergedWith(incompatible), "population merge rejection");
    incompatible = b;
    incompatible.metric.sessionId = "restart";
    require(!a.mergedWith(incompatible), "session merge rejection");
    incompatible = b;
    incompatible.metric.resetGeneration = 2;
    require(!a.mergedWith(incompatible), "reset generation merge rejection");
    incompatible = b;
    incompatible.metric.clockDomain = "jetson-steady";
    require(!a.mergedWith(incompatible), "cross-host clock merge rejection");
    incompatible = b;
    incompatible.windowEndNs = 3;
    require(!a.mergedWith(incompatible), "window merge rejection");
    incompatible = b;
    incompatible.histogram[histogramBucket(10)] = UINT64_MAX;
    require(!a.mergedWith(incompatible), "histogram overflow must reject merge");
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
    require(s.sampleCount == 0 && s.availability == Availability::Stale && !s.coverage(),
            "expired window stale, no fabricated healthy zero");
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
    require(r.availability == Availability::Stale && !r.ratePerSecond,
            "stale rate not represented as zero");
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
        testCaptureBenchmark();
        std::cout << "diagnostic_statistics_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "diagnostic_statistics_tests: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
