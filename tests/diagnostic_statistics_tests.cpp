#include "diagnostics/stats/Statistics.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace reg::diagnostics::stats;

namespace {
int checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                             " check failed: " #expression); \
} } while (false)

MetricIdentity identity(MetricKind kind, std::string population = "successful_exact_matches") {
    return {"reg.example", kind == MetricKind::Counter ? "frames" : "ns",
            "windows-reg", std::move(population), "reg-steady",
            "reg-session-1", "decoder-generation-1", kind};
}

void close(double actual, double expected, double tolerance) {
    CHECK(std::abs(actual - expected) < tolerance);
}

void duration_vector_and_outcomes() {
    DurationSeries series(identity(MetricKind::Duration));
    CHECK(series.record(1, Outcome::Success, 10));
    CHECK(series.record(2, Outcome::Success, 20));
    CHECK(series.record(3, Outcome::Success, 30));
    CHECK(series.record(4, Outcome::Success, 40));
    CHECK(series.record(5, Outcome::Timeout));
    CHECK(series.record(6, Outcome::Missing));
    CHECK(series.record(7, Outcome::Skipped));
    const auto snap = series.snapshot({0, 8}, 7);
    CHECK(snap.success_latency.sample_count == 4);
    CHECK(snap.success_latency.min_ns == 10);
    CHECK(snap.success_latency.max_ns == 40);
    CHECK(snap.success_latency.p50_ns == 20); // nearest-rank, not interpolated median
    CHECK(snap.success_latency.p95_ns == 40);
    CHECK(snap.success_latency.p99_ns == 40);
    close(*snap.success_latency.mean_ns, 25, 0.0001);
    CHECK(snap.success_latency.sum_ns == 100);
    CHECK(snap.missing == 1 && snap.timeout == 1 && snap.skipped == 1);
    CHECK(snap.eligible == 6);
    close(*snap.success_coverage, 4.0 / 6.0, 1e-10);
    CHECK(snap.quality.availability == Availability::Fresh);
    CHECK(snap.identity.population == "successful_exact_matches");
}

void synthetic_uniform_rates(int fps) {
    constexpr std::int64_t billion = 1'000'000'000;
    CounterSeries frames(identity(MetricKind::Counter));
    FrameIntervalSeries intervals(identity(MetricKind::FrameInterval, "decoded_frame_arrivals"));
    const std::int64_t samples = fps * 2;
    for (std::int64_t k = 0; k <= samples; ++k) {
        const std::int64_t time = (k * billion) / fps;
        CHECK(frames.record(time, static_cast<std::uint64_t>(k)));
        CHECK(intervals.record_frame(time));
    }
    const auto window = TimeWindow::last_seconds(2 * billion, 10);
    CHECK(window.has_value());
    const auto rate = frames.snapshot(*window, 2 * billion);
    CHECK(rate.status == RateStatus::Valid);
    CHECK(rate.observations == static_cast<std::uint64_t>(samples + 1));
    CHECK(rate.delta == static_cast<std::uint64_t>(samples));
    close(*rate.per_second, static_cast<double>(fps), 1e-9);
    const auto gaps = intervals.snapshot(*window, 2 * billion);
    CHECK(gaps.frames_in_window == static_cast<std::uint64_t>(samples + 1));
    CHECK(gaps.completed_intervals.sample_count == static_cast<std::uint64_t>(samples));
    CHECK(gaps.longest_completed_gap_ns.value_or(0) <= static_cast<std::uint64_t>(billion / fps + 1));
    CHECK(gaps.ongoing_gap_ns == 0);
    CHECK(gaps.quality.availability == Availability::Fresh);
}

void standard_windows_and_gaps() {
    constexpr std::int64_t billion = 1'000'000'000;
    constexpr int fps = 120;
    CounterSeries counter(identity(MetricKind::Counter));
    FrameIntervalSeries intervals(identity(MetricKind::FrameInterval, "decoded"));
    for (int k = 0; k <= 61 * fps; ++k) {
        const auto t = static_cast<std::int64_t>(k) * billion / fps;
        CHECK(counter.record(t, static_cast<std::uint64_t>(k)));
        CHECK(intervals.record_frame(t));
    }
    for (std::uint32_t seconds : {1u, 10u, 60u}) {
        const auto window = TimeWindow::last_seconds(61 * billion, seconds);
        CHECK(window.has_value());
        const auto rate = counter.snapshot(*window, 61 * billion);
        const auto pace = intervals.snapshot(*window, 61 * billion);
        CHECK(rate.status == RateStatus::Valid);
        CHECK(rate.delta == seconds * fps);
        CHECK(rate.observations == seconds * fps + 1);
        CHECK(pace.completed_intervals.sample_count == seconds * fps + 1);
        close(*rate.per_second, fps, 1e-9);
        CHECK(!pace.quality.history_truncated);
    }
    // A long *ongoing* stall must not masquerade as a successfully completed gap.
    const auto silent = intervals.snapshot({61 * billion + 1, 65 * billion + 1}, 65 * billion);
    CHECK(silent.frames_in_window == 0);
    CHECK(!silent.longest_completed_gap_ns);
    CHECK(silent.ongoing_gap_ns == 4 * billion);
    CHECK(silent.quality.availability == Availability::Stale);
}

void same_mean_bursty_gaps() {
    constexpr std::int64_t billion = 1'000'000'000;
    FrameIntervalSeries smooth(identity(MetricKind::FrameInterval, "decoded"));
    FrameIntervalSeries burst(identity(MetricKind::FrameInterval, "decoded"));
    CounterSeries smooth_count(identity(MetricKind::Counter));
    CounterSeries burst_count(identity(MetricKind::Counter));
    std::int64_t last_burst = 0;
    for (int k = 0; k <= 60; ++k) {
        const auto regular = static_cast<std::int64_t>(k) * billion / 60;
        if (k > 0) {
            // Four large gaps: 4*80ms + 56 short gaps totaling 680ms.
            last_burst = k <= 56 ? static_cast<std::int64_t>(k) * 680'000'000 / 56
                                 : 680'000'000 + static_cast<std::int64_t>(k - 56) * 80'000'000;
        }
        CHECK(smooth.record_frame(regular));
        CHECK(burst.record_frame(last_burst));
        CHECK(smooth_count.record(regular, k));
        CHECK(burst_count.record(last_burst, k));
    }
    const TimeWindow w{0, billion + 1};
    const auto s = smooth.snapshot(w, billion);
    const auto b = burst.snapshot(w, billion);
    CHECK(s.completed_intervals.sample_count == 60);
    CHECK(b.completed_intervals.sample_count == 60);
    CHECK(*b.completed_intervals.min_ns < *s.completed_intervals.min_ns);
    CHECK(*b.completed_intervals.max_ns > *s.completed_intervals.max_ns);
    CHECK(*b.completed_intervals.p95_ns > *s.completed_intervals.p95_ns);
    CHECK(*b.completed_intervals.p99_ns > *s.completed_intervals.p99_ns);
    close(*smooth_count.snapshot(w, billion).per_second, 60, 1e-9);
    close(*burst_count.snapshot(w, billion).per_second, 60, 1e-9);
    CHECK(b.longest_completed_gap_ns == 80'000'000);
}

void merge_scope_and_windows() {
    auto id = identity(MetricKind::Duration);
    DurationSeries left(id), right(id);
    CHECK(left.record(0, Outcome::Success, 8));
    CHECK(left.record(1, Outcome::Success, 12));
    CHECK(right.record(10, Outcome::Success, 40));
    CHECK(right.record(11, Outcome::Missing));
    auto a = left.snapshot({0, 10}, 19).sufficient();
    auto b = right.snapshot({10, 20}, 19).sufficient();
    auto merge = merge_sufficient(a, b);
    CHECK(merge.status == MergeStatus::Ok);
    CHECK(merge.merged->count == 3);
    CHECK(merge.merged->sum_ns == 60);
    CHECK(merge.merged->min_ns == 8 && merge.merged->max_ns == 40);
    CHECK(merge.merged->window.begin_ns == 0 && merge.merged->window.end_ns == 20);
    b.identity.session_id = "other-session";
    CHECK(merge_sufficient(a, b).status == MergeStatus::IncompatibleIdentity);
    b.identity = id;
    b.identity.population = "all_cv_packets";
    CHECK(merge_sufficient(a, b).status == MergeStatus::IncompatibleIdentity);
    b.identity = id;
    b.identity.reset_id = "generation-2";
    CHECK(merge_sufficient(a, b).status == MergeStatus::IncompatibleIdentity);
    b.identity = id;
    b.identity.clock_domain = "jetson-monotonic";
    CHECK(merge_sufficient(a, b).status == MergeStatus::IncompatibleIdentity);
    b.identity = id;
    b.window.begin_ns = 9;
    CHECK(merge_sufficient(a, b).status == MergeStatus::NonContiguousWindows);
    b.window.begin_ns = 11;
    CHECK(merge_sufficient(a, b).status == MergeStatus::NonContiguousWindows);
    b.window.begin_ns = 10;
    a.complete = false;
    CHECK(merge_sufficient(a, b).status == MergeStatus::IncompleteHistory);
}

void counter_staleness_restart_wrap_and_zeros() {
    CounterSeries series(identity(MetricKind::Counter), 32, 500);
    const auto empty = series.snapshot({0, 1}, 0);
    CHECK(empty.quality.availability == Availability::NeverObserved);
    CHECK(empty.status == RateStatus::InsufficientSamples);
    CHECK(!empty.per_second.has_value());
    CHECK(series.record(10, 100));
    CHECK(series.snapshot({0, 11}, 10).status == RateStatus::InsufficientSamples);
    CHECK(!series.record(10, 101));
    CHECK(!series.record(9, 101));
    CHECK(series.record(20, 100));
    const auto zero = series.snapshot({0, 21}, 20);
    CHECK(zero.status == RateStatus::Valid);
    CHECK(zero.delta == 0);
    CHECK(zero.per_second == 0.0);
    CHECK(series.record(30, 120));
    CHECK(series.record(40, 4)); // reset, do not wrap counter
    CHECK(series.record(50, 8));
    const auto crossing = series.snapshot({10, 51}, 50);
    CHECK(crossing.resets_in_window == 1);
    CHECK(crossing.status == RateStatus::ResetBoundary);
    CHECK(!crossing.per_second.has_value());
    const auto after = series.snapshot({41, 51}, 50);
    CHECK(after.status == RateStatus::InsufficientSamples);
    CHECK(series.record(60, 14));
    const auto clean = series.snapshot({40, 61}, 60);
    CHECK(clean.status == RateStatus::ResetBoundary); // explicit reset sample belongs to window
    const auto clean_after = series.snapshot({41, 61}, 60);
    CHECK(clean_after.status == RateStatus::Valid);
    CHECK(clean_after.delta == 6);
    close(*clean_after.per_second, 600'000'000.0, 0.1);
    CHECK(series.snapshot({0, 61}, 1000).quality.availability == Availability::Stale);
    series.set_availability(Availability::Disabled);
    CHECK(!series.record(1001, 15));
    CHECK(series.snapshot({0, 1002}, 1001).quality.availability == Availability::Disabled);
    series.set_availability(Availability::Error);
    CHECK(series.snapshot({0, 1002}, 1001).quality.availability == Availability::Error);

    CounterSeries max_counter(identity(MetricKind::Counter));
    CHECK(max_counter.record(0, std::numeric_limits<std::uint64_t>::max() - 1));
    CHECK(max_counter.record(1'000'000'000, std::numeric_limits<std::uint64_t>::max()));
    const auto high = max_counter.snapshot({0, 1'000'000'001}, 1'000'000'000);
    CHECK(high.delta == 1);
    close(*high.per_second, 1.0, 1e-12);
    CHECK(max_counter.record(2'000'000'000, 0));
    CHECK(max_counter.snapshot({0, 2'000'000'001}, 2'000'000'000).status == RateStatus::ResetBoundary);
}

void expired_window_and_provenance() {
    DurationSeries series(identity(MetricKind::Duration), 3, 100);
    for (int i = 0; i < 5; ++i) CHECK(series.record(i * 10, Outcome::Success, i + 1));
    const auto truncated = series.snapshot({0, 41}, 40);
    CHECK(truncated.quality.history_truncated);
    CHECK(truncated.success_latency.sample_count == 3);
    CHECK(!truncated.sufficient().complete);
    const auto tail = series.snapshot({25, 41}, 40);
    CHECK(!tail.quality.history_truncated);
    CHECK(tail.success_latency.sample_count == 2);
    CHECK(tail.success_latency.p50_ns == 4);
    const auto expired = series.snapshot({41, 51}, 50);
    CHECK(expired.success_latency.sample_count == 0);
    CHECK(!expired.success_latency.mean_ns);
    CHECK(!expired.success_coverage);
    CHECK(expired.quality.availability == Availability::Fresh);
    CHECK(series.snapshot({41, 201}, 200).quality.availability == Availability::Stale);
    CHECK(series.reserved_bytes() == 3 * 24 || series.reserved_bytes() > 0);
}

void invalid_inputs_and_large_durations() {
    DurationSeries series(identity(MetricKind::Duration));
    CHECK(!series.record(-1, Outcome::Success, 10));
    CHECK(!series.record(0, static_cast<Outcome>(255), 10));
    CHECK(!series.record(0, Outcome::Success));
    CHECK(!series.record(0, Outcome::Timeout, 42));
    CHECK(!series.record_double(0, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!series.record_double(0, std::numeric_limits<double>::infinity()));
    CHECK(!series.record_double(0, -1.0));
    CHECK(!series.record_double(0, 1.5));
    CHECK(!series.record_double(0, std::ldexp(1.0, 64)));
    CHECK(series.record(0, Outcome::Success, std::numeric_limits<std::uint64_t>::max()));
    CHECK(series.record(1, Outcome::Success, std::numeric_limits<std::uint64_t>::max()));
    CHECK(!series.record(1, Outcome::Success, 6));
    CHECK(!series.record(0, Outcome::Success, 6));
    const auto snap = series.snapshot({0, 2}, 1);
    CHECK(snap.quality.rejected_inputs == 11);
    CHECK(snap.success_latency.sample_count == 2);
    CHECK(snap.success_latency.max_ns == std::numeric_limits<std::uint64_t>::max());
    CHECK(std::isfinite(*snap.success_latency.mean_ns));
    FrameIntervalSeries pacing(identity(MetricKind::FrameInterval));
    CHECK(!pacing.record_frame(-3));
    CHECK(pacing.record_frame(0));
    CHECK(!pacing.record_frame(0));
    CHECK(pacing.record_frame(10));
    CHECK(pacing.snapshot({0, 11}, 10).completed_intervals.p99_ns == 10);
    bool threw = false;
    try { (void)series.snapshot({10, 10}, 10); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { (void)series.snapshot({0, 100}, 0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    CHECK(!TimeWindow::last_seconds(-1, 1));
    CHECK(!TimeWindow::last_seconds(INT64_MAX, 60));
    CHECK(!TimeWindow::last_seconds(0, 0));
}

void unavailable_and_independent_ownership() {
    DurationSeries no_data(identity(MetricKind::Duration));
    const auto initial = no_data.snapshot({0, 1}, 0);
    CHECK(initial.quality.availability == Availability::NeverObserved);
    CHECK(initial.eligible == 0);
    CHECK(!initial.success_coverage);
    no_data.set_availability(Availability::Unsupported);
    CHECK(no_data.snapshot({0, 1}, 0).quality.availability == Availability::Unsupported);
    CHECK(!no_data.record(0, Outcome::Success, 0));
    no_data.set_availability(Availability::Fresh);
    CHECK(no_data.record(0, Outcome::Success, 0)); // measured zero is valid
    const auto zero = no_data.snapshot({0, 1}, 0);
    CHECK(zero.success_latency.sample_count == 1);
    CHECK(zero.success_latency.p99_ns == 0);
    CHECK(zero.success_coverage == 1.0);

    // Independent per-owner monotonic clocks must never be rate-joined.
    auto remote_id = identity(MetricKind::Duration);
    remote_id.owner = "jetson";
    DurationSeries remote(remote_id);
    CHECK(remote.record(0, Outcome::Success, 1));
    CHECK(merge_sufficient(zero.sufficient(), remote.snapshot({1, 2}, 1).sufficient()).status ==
          MergeStatus::IncompatibleIdentity);
}

void stress_bounded_capture() {
    constexpr int count = 300'000;
    constexpr int capacity = 4096;
    DurationSeries enabled(identity(MetricKind::Duration), capacity);
    DurationSeries disabled(identity(MetricKind::Duration), capacity);
    disabled.set_availability(Availability::Disabled);
    const auto run = [&](DurationSeries& target) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
            const auto kind = i % 7 == 0 ? Outcome::Missing :
                              i % 11 == 0 ? Outcome::Timeout : Outcome::Success;
            const auto optional_ns = kind == Outcome::Success
                ? std::optional<std::uint64_t>(100 + static_cast<std::uint64_t>(i % 17))
                : std::nullopt;
            (void)target.record(static_cast<std::int64_t>(i) * 8'333'333, kind, optional_ns);
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    };
    const auto enabled_ns = run(enabled);
    const auto disabled_ns = run(disabled);
    constexpr std::int64_t last_ns = static_cast<std::int64_t>(count - 1) * 8'333'333;
    const auto w = TimeWindow::last_seconds(last_ns, 10);
    CHECK(w.has_value());
    const auto s = enabled.snapshot(*w, last_ns);
    CHECK(s.eligible > 0);
    CHECK(s.success_latency.sample_count > 0);
    CHECK(s.quality.history_truncated == false); // >4096 capacity for the final 10 s
    CHECK(disabled.snapshot(*w, last_ns).quality.availability == Availability::Disabled);
    CHECK(enabled.reserved_bytes() <= capacity * 32u);
    CHECK(enabled_ns > 0 && disabled_ns > 0);
    std::cout << "Synthetic capture throughput: enabled="
              << static_cast<double>(count) * 1e9 / static_cast<double>(enabled_ns)
              << " samples/s, disabled="
              << static_cast<double>(count) * 1e9 / static_cast<double>(disabled_ns)
              << " samples/s; preallocated=" << enabled.reserved_bytes() << " B\n";
}
} // namespace

int main() {
    try {
        duration_vector_and_outcomes();
        synthetic_uniform_rates(60);
        synthetic_uniform_rates(120);
        standard_windows_and_gaps();
        same_mean_bursty_gaps();
        merge_scope_and_windows();
        counter_staleness_restart_wrap_and_zeros();
        expired_window_and_provenance();
        invalid_inputs_and_large_durations();
        unavailable_and_independent_ownership();
        stress_bounded_capture();
        std::cout << "PASS: " << checks << " diagnostic statistics assertions\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
