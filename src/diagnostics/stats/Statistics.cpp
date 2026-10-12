#include "diagnostics/stats/Statistics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace reg::diagnostics::stats {
namespace {

void validate(const MetricIdentity& id, MetricKind expected, std::size_t capacity,
              std::int64_t stale_ns) {
    if (id.kind != expected || id.name.empty() || id.unit.empty() ||
        id.owner.empty() || id.population.empty() || id.clock_domain.empty() ||
        id.session_id.empty() || id.reset_id.empty() || capacity == 0 || stale_ns < 0) {
        throw std::invalid_argument("invalid metric identity, capacity or staleness threshold");
    }
}

Availability current_availability(Availability external, std::optional<std::int64_t> last,
                                  std::int64_t now, std::int64_t stale_after) noexcept {
    if (external != Availability::Fresh) return external;
    if (!last) return Availability::NeverObserved;
    if (now < *last || now < 0) return Availability::Error;
    return now - *last > stale_after ? Availability::Stale : Availability::Fresh;
}

// Exact uint64 order statistics (nearest rank), never average or re-rank P95s.
Distribution distribution(std::vector<std::uint64_t>& samples) {
    Distribution out;
    if (samples.empty()) return out;
    std::sort(samples.begin(), samples.end()); // only during explicit snapshot
    out.sample_count = static_cast<std::uint64_t>(samples.size());
    out.min_ns = samples.front();
    out.max_ns = samples.back();
    for (std::uint64_t v : samples) out.sum_ns += static_cast<long double>(v);
    out.mean_ns = static_cast<double>(out.sum_ns / static_cast<long double>(samples.size()));
    const auto rank = [&](std::size_t numerator, std::size_t denominator) -> std::uint64_t {
        // ceil(n*p)-1 without multiplication overflow (n <= ring capacity).
        const std::size_t index = (samples.size() / denominator) * numerator +
            ((samples.size() % denominator) * numerator + denominator - 1) / denominator - 1;
        return samples[index];
    };
    out.p50_ns = rank(50, 100);
    out.p95_ns = rank(95, 100);
    out.p99_ns = rank(99, 100);
    return out;
}

bool history_truncated(std::optional<std::int64_t> evicted, TimeWindow w) noexcept {
    return evicted && *evicted >= w.begin_ns;
}

void validate_window(TimeWindow w, std::int64_t now) {
    if (!w.valid() || now < 0 || w.end_ns > now + (now != INT64_MAX ? 1 : 0)) {
        throw std::invalid_argument("invalid or future time window");
    }
}

} // namespace

std::optional<TimeWindow> TimeWindow::last_seconds(std::int64_t now,
                                                   std::uint32_t seconds) noexcept {
    if (now < 0 || now == INT64_MAX || seconds == 0) return std::nullopt;
    constexpr std::int64_t billion = 1'000'000'000;
    const std::int64_t span = static_cast<std::int64_t>(seconds) * billion;
    return TimeWindow{now > span ? now - span : 0, now + 1};
}

MergeResult merge_sufficient(const SufficientStats& left,
                             const SufficientStats& right) noexcept {
    if (!(left.identity == right.identity)) return {MergeStatus::IncompatibleIdentity, {}};
    if (!left.complete || !right.complete) return {MergeStatus::IncompleteHistory, {}};
    if (!left.window.valid() || !right.window.valid() ||
        left.window.end_ns != right.window.begin_ns) {
        return {MergeStatus::NonContiguousWindows, {}};
    }
    if (std::numeric_limits<std::uint64_t>::max() - left.count < right.count) {
        return {MergeStatus::IncompleteHistory, {}};
    }
    SufficientStats out{};
    out.identity = left.identity;
    out.window = {left.window.begin_ns, right.window.end_ns};
    out.count = left.count + right.count;
    out.sum_ns = left.sum_ns + right.sum_ns;
    if (left.min_ns && right.min_ns) out.min_ns = std::min(*left.min_ns, *right.min_ns);
    else out.min_ns = left.min_ns ? left.min_ns : right.min_ns;
    if (left.max_ns && right.max_ns) out.max_ns = std::max(*left.max_ns, *right.max_ns);
    else out.max_ns = left.max_ns ? left.max_ns : right.max_ns;
    return {MergeStatus::Ok, std::move(out)};
}

SufficientStats DurationSnapshot::sufficient() const {
    return {identity, window, success_latency.sample_count, success_latency.sum_ns,
            success_latency.min_ns, success_latency.max_ns,
            !quality.history_truncated && quality.availability != Availability::Error &&
                quality.availability != Availability::Unsupported};
}

DurationSeries::DurationSeries(MetricIdentity id, std::size_t cap, std::int64_t stale)
    : identity_(std::move(id)), history_(cap), stale_after_ns_(stale) {
    validate(identity_, MetricKind::Duration, cap, stale);
}

bool DurationSeries::record(std::int64_t t, Outcome outcome,
                            std::optional<std::uint64_t> duration) noexcept {
    if (override_ == Availability::Disabled || override_ == Availability::Unsupported ||
        t < 0 || (last_time_ && t <= *last_time_) ||
        (outcome != Outcome::Success && outcome != Outcome::Missing &&
         outcome != Outcome::Timeout && outcome != Outcome::Skipped) ||
        (outcome == Outcome::Success) != duration.has_value()) {
        ++rejected_;
        return false;
    }
    if (history_.size() == history_.capacity()) last_evicted_time_ = history_.at(0).t;
    history_.push({t, duration.value_or(0), outcome});
    last_time_ = t;
    override_ = Availability::Fresh;
    return true;
}

bool DurationSeries::record_double(std::int64_t t, double duration) noexcept {
    if (!std::isfinite(duration) || duration < 0 || std::trunc(duration) != duration ||
        static_cast<long double>(duration) >= std::ldexp(1.0L, 64)) {
        ++rejected_;
        return false;
    }
    return record(t, Outcome::Success, static_cast<std::uint64_t>(duration));
}

void DurationSeries::set_availability(Availability a) noexcept { override_ = a; }
std::size_t DurationSeries::reserved_bytes() const noexcept {
    return history_.capacity() * sizeof(Sample);
}

DurationSnapshot DurationSeries::snapshot(TimeWindow w, std::int64_t now) const {
    validate_window(w, now);
    DurationSnapshot out;
    out.identity = identity_;
    out.window = w;
    out.quality = {current_availability(override_, last_time_, now, stale_after_ns_),
                   history_truncated(last_evicted_time_, w), rejected_};
    std::vector<std::uint64_t> successful;
    successful.reserve(history_.size());
    for (std::size_t i = 0; i < history_.size(); ++i) {
        const auto& sample = history_.at(i);
        if (!w.contains(sample.t)) continue;
        switch (sample.outcome) {
        case Outcome::Success: successful.push_back(sample.duration); break;
        case Outcome::Missing: ++out.missing; break;
        case Outcome::Timeout: ++out.timeout; break;
        case Outcome::Skipped: ++out.skipped; break;
        }
    }
    out.success_latency = distribution(successful);
    out.eligible = out.success_latency.sample_count + out.missing + out.timeout;
    if (out.eligible > 0) {
        out.success_coverage = static_cast<double>(out.success_latency.sample_count) /
                               static_cast<double>(out.eligible);
    }
    return out;
}

CounterSeries::CounterSeries(MetricIdentity id, std::size_t cap, std::int64_t stale)
    : identity_(std::move(id)), history_(cap), stale_after_ns_(stale) {
    validate(identity_, MetricKind::Counter, cap, stale);
}

bool CounterSeries::record(std::int64_t t, std::uint64_t value) noexcept {
    if (override_ == Availability::Disabled || override_ == Availability::Unsupported ||
        t < 0 || (last_time_ && t <= *last_time_)) {
        ++rejected_;
        return false;
    }
    const bool reset = last_time_ && value < last_counter_;
    if (reset && segment_ == std::numeric_limits<std::uint64_t>::max()) {
        ++rejected_;
        return false;
    }
    if (reset) ++segment_;
    if (history_.size() == history_.capacity()) last_evicted_time_ = history_.at(0).t;
    history_.push({t, value, segment_, reset});
    last_time_ = t;
    last_counter_ = value;
    override_ = Availability::Fresh;
    return true;
}

void CounterSeries::set_availability(Availability a) noexcept { override_ = a; }
std::size_t CounterSeries::reserved_bytes() const noexcept {
    return history_.capacity() * sizeof(Sample);
}

CounterSnapshot CounterSeries::snapshot(TimeWindow w, std::int64_t now) const {
    validate_window(w, now);
    CounterSnapshot out;
    out.identity = identity_;
    out.window = w;
    out.quality = {current_availability(override_, last_time_, now, stale_after_ns_),
                   history_truncated(last_evicted_time_, w), rejected_};
    std::optional<Sample> first;
    std::optional<Sample> last;
    for (std::size_t i = 0; i < history_.size(); ++i) {
        const auto& v = history_.at(i);
        if (!w.contains(v.t)) continue;
        ++out.observations;
        if (v.reset) ++out.resets_in_window;
        if (!first) first = v;
        last = v;
    }
    if (out.quality.history_truncated) out.status = RateStatus::IncompleteHistory;
    else if (out.resets_in_window > 0 || (first && last && first->segment != last->segment)) {
        out.status = RateStatus::ResetBoundary;
    } else if (out.observations < 2) {
        out.status = RateStatus::InsufficientSamples;
    } else {
        const auto elapsed = last->t - first->t;
        if (elapsed <= 0) return out; // strictly monotonic ingestion makes this unreachable
        out.status = RateStatus::Valid;
        out.elapsed_ns = elapsed;
        out.delta = last->value - first->value;
        out.per_second = static_cast<double>(
            static_cast<long double>(*out.delta) * 1'000'000'000.0L /
            static_cast<long double>(elapsed));
    }
    return out;
}

FrameIntervalSeries::FrameIntervalSeries(MetricIdentity id, std::size_t cap,
                                         std::int64_t stale)
    : identity_(std::move(id)), history_(cap), stale_after_ns_(stale) {
    validate(identity_, MetricKind::FrameInterval, cap, stale);
}

bool FrameIntervalSeries::record_frame(std::int64_t t) noexcept {
    if (override_ == Availability::Disabled || override_ == Availability::Unsupported ||
        t < 0 || (last_time_ && t <= *last_time_)) {
        ++rejected_;
        return false;
    }
    const Sample sample{t, last_time_ ? static_cast<std::uint64_t>(t - *last_time_) : 0,
                        last_time_.has_value()};
    if (history_.size() == history_.capacity()) last_evicted_time_ = history_.at(0).t;
    history_.push(sample);
    last_time_ = t;
    override_ = Availability::Fresh;
    return true;
}

void FrameIntervalSeries::set_availability(Availability a) noexcept { override_ = a; }
std::size_t FrameIntervalSeries::reserved_bytes() const noexcept {
    return history_.capacity() * sizeof(Sample);
}

FrameSnapshot FrameIntervalSeries::snapshot(TimeWindow w, std::int64_t now) const {
    validate_window(w, now);
    FrameSnapshot out;
    out.identity = identity_;
    out.window = w;
    out.quality = {current_availability(override_, last_time_, now, stale_after_ns_),
                   history_truncated(last_evicted_time_, w), rejected_};
    std::vector<std::uint64_t> gaps;
    gaps.reserve(history_.size());
    for (std::size_t i = 0; i < history_.size(); ++i) {
        const auto& s = history_.at(i);
        if (!w.contains(s.t)) continue;
        ++out.frames_in_window;
        if (s.has_interval) gaps.push_back(s.interval);
    }
    out.completed_intervals = distribution(gaps);
    out.longest_completed_gap_ns = out.completed_intervals.max_ns;
    if (last_time_ && now >= *last_time_) {
        out.ongoing_gap_ns = static_cast<std::uint64_t>(now - *last_time_);
    }
    return out;
}

} // namespace reg::diagnostics::stats
