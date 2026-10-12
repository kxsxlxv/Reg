#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace reg::diagnostics::stats {

// All timestamps are nonnegative nanoseconds on ONE owner-local monotonic clock.
// Cross-host monotonic timestamp subtraction is never valid.
enum class MetricKind { Counter, Duration, FrameInterval };
enum class Availability { Fresh, Stale, NeverObserved, Disabled, Error, Unsupported };
enum class Outcome { Success, Missing, Timeout, Skipped };
enum class RateStatus { Valid, InsufficientSamples, ResetBoundary, IncompleteHistory };
enum class MergeStatus { Ok, IncompatibleIdentity, NonContiguousWindows, IncompleteHistory };

struct MetricIdentity {
    std::string name;
    std::string unit;
    std::string owner;
    std::string population;
    std::string clock_domain;
    std::string session_id;
    std::string reset_id;
    MetricKind kind{MetricKind::Duration};
    bool operator==(const MetricIdentity&) const = default;
};

// Half-open [begin_ns, end_ns). A snapshot retains these exact boundaries.
struct TimeWindow {
    std::int64_t begin_ns{};
    std::int64_t end_ns{};
    [[nodiscard]] bool valid() const noexcept { return begin_ns >= 0 && end_ns > begin_ns; }
    [[nodiscard]] bool contains(std::int64_t ns) const noexcept {
        return ns >= begin_ns && ns < end_ns;
    }
    // Includes an observation at now_ns; returns nullopt on invalid/overflow.
    [[nodiscard]] static std::optional<TimeWindow> last_seconds(
        std::int64_t now_ns, std::uint32_t seconds) noexcept;
};

struct Quality {
    Availability availability{Availability::NeverObserved};
    bool history_truncated{};
    std::uint64_t rejected_inputs{}; // monotonic since object creation, not a per-window rate
};

struct Distribution {
    std::uint64_t sample_count{};
    long double sum_ns{}; // sufficient statistic; extreme values may round on MSVC
    std::optional<std::uint64_t> min_ns;
    std::optional<std::uint64_t> max_ns;
    std::optional<double> mean_ns;
    // Exact nearest-rank order statistics of retained successful observations.
    // p99 with fewer than 100 samples is normally the maximum; expose count.
    std::optional<std::uint64_t> p50_ns;
    std::optional<std::uint64_t> p95_ns;
    std::optional<std::uint64_t> p99_ns;
};

struct SufficientStats {
    MetricIdentity identity;
    TimeWindow window;
    std::uint64_t count{};
    long double sum_ns{};
    std::optional<std::uint64_t> min_ns;
    std::optional<std::uint64_t> max_ns;
    bool complete{true};
};

struct MergeResult {
    MergeStatus status{MergeStatus::IncompatibleIdentity};
    std::optional<SufficientStats> merged;
};

// Only sufficient stats can be merged; quantiles cannot be merged or averaged.
// Requires exact identity (owner, population, clock, session, reset and unit),
// contiguous half-open windows, and no capacity-related history loss.
[[nodiscard]] MergeResult merge_sufficient(const SufficientStats& left,
                                           const SufficientStats& right) noexcept;

struct DurationSnapshot {
    MetricIdentity identity;
    TimeWindow window;
    Quality quality;
    Distribution success_latency;
    std::uint64_t missing{};
    std::uint64_t timeout{};
    std::uint64_t skipped{};
    std::uint64_t eligible{}; // success + missing + timeout; skipped NOT eligible
    std::optional<double> success_coverage; // success / eligible; null if eligible=0
    [[nodiscard]] SufficientStats sufficient() const;
};

struct CounterSnapshot {
    MetricIdentity identity;
    TimeWindow window;
    Quality quality;
    RateStatus status{RateStatus::InsufficientSamples};
    std::uint64_t observations{};
    std::uint64_t resets_in_window{};
    std::optional<std::uint64_t> delta;
    std::optional<double> per_second;
    std::optional<std::int64_t> elapsed_ns;
};

struct FrameSnapshot {
    MetricIdentity identity;
    TimeWindow window;
    Quality quality;
    std::uint64_t frames_in_window{};
    Distribution completed_intervals; // interval assigned to its ending frame
    std::optional<std::uint64_t> longest_completed_gap_ns;
    std::optional<std::uint64_t> ongoing_gap_ns; // now - most recent frame, even if no new frame
};

// Fixed-size ring: reserve once in constructor; ingestion never allocates.
// Single producer / snapshot must be externally serialized or run on a non-RT
// observer thread. Do not call snapshot/quantiles from the decode/render hot path.
template <typename T> class BoundedRing {
public:
    explicit BoundedRing(std::size_t capacity) : slots_(capacity) {}
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const T& at(std::size_t index) const noexcept {
        return slots_[(head_ + index) % slots_.size()];
    }
    void push(const T& sample) noexcept {
        if (size_ < slots_.size()) {
            slots_[(head_ + size_) % slots_.size()] = sample;
            ++size_;
        } else {
            slots_[head_] = sample;
            head_ = (head_ + 1) % slots_.size();
        }
    }
private:
    std::vector<T> slots_;
    std::size_t head_{};
    std::size_t size_{};
};

class DurationSeries {
public:
    explicit DurationSeries(MetricIdentity identity, std::size_t capacity = 8192,
                            std::int64_t stale_after_ns = 2'000'000'000);
    // Success requires a measured duration, all other outcomes require nullopt.
    // false means the observation was rejected, not converted to a zero sample.
    bool record(std::int64_t time_ns, Outcome outcome,
                std::optional<std::uint64_t> duration_ns = std::nullopt) noexcept;
    // Strict double adapter: rejects NaN, infinity, negative, fractions and >= 2^64.
    bool record_double(std::int64_t time_ns, double duration_ns) noexcept;
    void set_availability(Availability availability) noexcept;
    [[nodiscard]] DurationSnapshot snapshot(TimeWindow window,
                                            std::int64_t now_ns) const;
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;
private:
    struct Sample {
        std::int64_t t{};
        std::uint64_t duration{};
        Outcome outcome{Outcome::Skipped};
    };
    MetricIdentity identity_;
    BoundedRing<Sample> history_;
    std::int64_t stale_after_ns_;
    std::optional<std::int64_t> last_time_;
    std::optional<std::int64_t> last_evicted_time_;
    std::uint64_t rejected_{};
    Availability override_{Availability::Fresh};
};

class CounterSeries {
public:
    explicit CounterSeries(MetricIdentity identity, std::size_t capacity = 8192,
                           std::int64_t stale_after_ns = 2'000'000'000);
    bool record(std::int64_t time_ns, std::uint64_t counter) noexcept;
    void set_availability(Availability availability) noexcept;
    [[nodiscard]] CounterSnapshot snapshot(TimeWindow window,
                                           std::int64_t now_ns) const;
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;
private:
    struct Sample {
        std::int64_t t{};
        std::uint64_t value{};
        std::uint64_t segment{};
        bool reset{};
    };
    MetricIdentity identity_;
    BoundedRing<Sample> history_;
    std::int64_t stale_after_ns_;
    std::optional<std::int64_t> last_time_;
    std::optional<std::int64_t> last_evicted_time_;
    std::uint64_t last_counter_{};
    std::uint64_t segment_{};
    std::uint64_t rejected_{};
    Availability override_{Availability::Fresh};
};

class FrameIntervalSeries {
public:
    explicit FrameIntervalSeries(MetricIdentity identity, std::size_t capacity = 8192,
                                 std::int64_t stale_after_ns = 2'000'000'000);
    bool record_frame(std::int64_t time_ns) noexcept;
    void set_availability(Availability availability) noexcept;
    [[nodiscard]] FrameSnapshot snapshot(TimeWindow window,
                                         std::int64_t now_ns) const;
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;
private:
    struct Sample {
        std::int64_t t{};
        std::uint64_t interval{};
        bool has_interval{};
    };
    MetricIdentity identity_;
    BoundedRing<Sample> history_;
    std::int64_t stale_after_ns_;
    std::optional<std::int64_t> last_time_;
    std::optional<std::int64_t> last_evicted_time_;
    std::uint64_t rejected_{};
    Availability override_{Availability::Fresh};
};

} // namespace reg::diagnostics::stats
