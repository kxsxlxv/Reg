#pragma once

// Dependency-free, single-writer statistics for Reg's off-critical-path collectors.
// C++23; no wall clocks, global state, network, GPU, JSON or hot-path sorting.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reg::diagnostics::stats {

enum class MetricKind { Duration, FrameInterval, Counter };
enum class Availability { Unsupported, Unavailable, Fresh, Stale };
enum class Outcome : std::uint8_t { Success, Timeout, Missing, Skipped, Cancelled, Late, Count };

struct MetricIdentity {
    std::string name;
    std::string unit;
    std::string owner;
    std::string population;
    std::string clockDomain;
    std::string sessionId;
    std::uint64_t resetGeneration{};
    MetricKind kind{MetricKind::Duration};
    bool supported{true};
    bool operator==(const MetricIdentity&) const = default;
};

inline constexpr std::size_t kHistogramBuckets = 1024;
inline constexpr std::size_t kOutcomeCount = static_cast<std::size_t>(Outcome::Count);
inline constexpr std::int64_t kOneSecondNs = 1'000'000'000;
inline constexpr std::int64_t kTenSecondsNs = 10 * kOneSecondNs;
inline constexpr std::int64_t kSixtySecondsNs = 60 * kOneSecondNs;

// Relative bin width <= 1/16 per octave (max upper-bound overestimate < 6.25%).
// Values [0, 15] ns are exact. All uint64 values, including UINT64_MAX, fit.
inline std::size_t histogramBucket(std::uint64_t value) noexcept {
    if (value < 16) return static_cast<std::size_t>(value);
    const auto exponent = 63U - static_cast<unsigned>(std::countl_zero(value));
    const auto base = std::uint64_t{1} << exponent;
    const auto offset = (value - base) >> (exponent - 4);
    return static_cast<std::size_t>(exponent * 16 + offset);
}

inline std::uint64_t histogramUpperBound(std::size_t bucket) noexcept {
    if (bucket < 16) return static_cast<std::uint64_t>(bucket);
    const auto exponent = static_cast<unsigned>(bucket / 16);
    const auto subdivision = static_cast<unsigned>(bucket % 16);
    if (exponent == 63 && subdivision == 15) return UINT64_MAX;
    return (std::uint64_t{1} << exponent) +
           ((static_cast<std::uint64_t>(subdivision) + 1) << (exponent - 4)) - 1;
}

struct DistributionSnapshot {
    MetricIdentity metric;
    std::int64_t windowStartNs{}; // inclusive, owner-local monotonic domain
    std::int64_t windowEndNs{};   // inclusive
    Availability availability{Availability::Unavailable};
    std::optional<std::int64_t> lastObservationNs;
    std::uint64_t rejectedInputs{}; // total over stream lifetime (not window-local)
    std::array<std::uint64_t, kOutcomeCount> outcomes{};
    std::array<std::uint64_t, kHistogramBuckets> histogram{};
    std::uint64_t sampleCount{};
    long double sumNs{};
    std::optional<std::uint64_t> minNs;
    std::optional<std::uint64_t> maxNs;

    void addDuration(std::uint64_t ns) noexcept {
        ++sampleCount;
        ++outcomes[static_cast<std::size_t>(Outcome::Success)];
        sumNs += static_cast<long double>(ns);
        if (!minNs || ns < *minNs) minNs = ns;
        if (!maxNs || ns > *maxNs) maxNs = ns;
        ++histogram[histogramBucket(ns)];
    }

    void addOutcome(Outcome outcome) noexcept {
        if (outcome != Outcome::Success && outcome != Outcome::Count)
            ++outcomes[static_cast<std::size_t>(outcome)];
    }

    [[nodiscard]] std::uint64_t observedCount() const noexcept {
        std::uint64_t n = 0;
        for (auto v : outcomes) n += v;
        return n;
    }

    [[nodiscard]] std::optional<long double> meanNs() const noexcept {
        if (!sampleCount) return std::nullopt;
        return sumNs / static_cast<long double>(sampleCount);
    }

    [[nodiscard]] std::optional<double> coverage() const noexcept {
        const auto n = observedCount();
        if (!n) return std::nullopt;
        return static_cast<double>(sampleCount) / static_cast<double>(n);
    }

    // Nearest-rank percentile; conservative upper endpoint of logarithmic bin.
    // For 1-s windows P99 with ~60 samples is descriptive, not a stable tail.
    [[nodiscard]] std::optional<std::uint64_t> percentileUpper(double percentile) const noexcept {
        if (!sampleCount || !std::isfinite(percentile) || percentile < 0 || percentile > 100)
            return std::nullopt;
        const auto rank = std::max<std::uint64_t>(1,
            static_cast<std::uint64_t>(std::ceil(percentile / 100.0L * sampleCount)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < histogram.size(); ++i) {
            seen += histogram[i];
            if (seen >= rank) return histogramUpperBound(i);
        }
        return std::nullopt;
    }

    // Never merge unlike populations/clock/session/reset/metric/window domains.
    // Caller guarantees disjoint observations (not two copies of one stream).
    [[nodiscard]] std::optional<DistributionSnapshot> mergedWith(
        const DistributionSnapshot& other) const {
        if (!(metric == other.metric) || windowStartNs != other.windowStartNs ||
            windowEndNs != other.windowEndNs || availability == Availability::Unsupported ||
            other.availability == Availability::Unsupported ||
            sampleCount > UINT64_MAX - other.sampleCount ||
            rejectedInputs > UINT64_MAX - other.rejectedInputs) return std::nullopt;
        for (std::size_t i = 0; i < kHistogramBuckets; ++i)
            if (histogram[i] > UINT64_MAX - other.histogram[i]) return std::nullopt;
        for (std::size_t i = 0; i < kOutcomeCount; ++i)
            if (outcomes[i] > UINT64_MAX - other.outcomes[i]) return std::nullopt;
        DistributionSnapshot merged = *this;
        merged.sampleCount += other.sampleCount;
        merged.sumNs += other.sumNs;
        merged.rejectedInputs += other.rejectedInputs;
        for (std::size_t i = 0; i < kHistogramBuckets; ++i)
            merged.histogram[i] += other.histogram[i];
        for (std::size_t i = 0; i < kOutcomeCount; ++i)
            merged.outcomes[i] += other.outcomes[i];
        if (other.minNs && (!merged.minNs || *other.minNs < *merged.minNs))
            merged.minNs = other.minNs;
        if (other.maxNs && (!merged.maxNs || *other.maxNs > *merged.maxNs))
            merged.maxNs = other.maxNs;
        if (other.lastObservationNs &&
            (!merged.lastObservationNs || *other.lastObservationNs > *merged.lastObservationNs))
            merged.lastObservationNs = other.lastObservationNs;
        if (availability == Availability::Stale || other.availability == Availability::Stale)
            merged.availability = Availability::Stale;
        else if (availability == Availability::Fresh || other.availability == Availability::Fresh)
            merged.availability = Availability::Fresh;
        return merged;
    }
};

inline void validateConfig(std::size_t capacity, std::int64_t freshnessNs) {
    if (capacity < 2 || freshnessNs <= 0)
        throw std::invalid_argument("stats capacity >=2 and positive freshness required");
}

// Fixed-size preallocated FIFO: insertion has O(1) cost, no allocation.
template <typename T>
class Ring final {
public:
    explicit Ring(std::size_t capacity) : data_(capacity) {}
    void clear() noexcept { head_ = size_ = 0; }
    void push(const T& entry) noexcept {
        if (size_ < data_.size()) {
            data_[(head_ + size_) % data_.size()] = entry;
            ++size_;
        } else {
            data_[head_] = entry;
            head_ = (head_ + 1) % data_.size();
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const T& at(std::size_t i) const noexcept {
        return data_[(head_ + i) % data_.size()];
    }
    [[nodiscard]] const T& back() const noexcept { return at(size_ - 1); }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return data_.size() * sizeof(T); }
private:
    std::vector<T> data_;
    std::size_t head_{};
    std::size_t size_{};
};

inline Availability determineAvailability(std::optional<std::int64_t> last,
                                          std::int64_t now, std::int64_t freshnessNs) noexcept {
    if (!last || now < *last) return Availability::Unavailable;
    const auto elapsed = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(*last);
    return elapsed > static_cast<std::uint64_t>(freshnessNs) ? Availability::Stale : Availability::Fresh;
}

class DurationWindow final {
    struct Entry { std::int64_t time; std::uint64_t ns; Outcome outcome; };
public:
    explicit DurationWindow(MetricIdentity identity, std::size_t capacity = 16384,
                            std::int64_t freshnessNs = 2 * kOneSecondNs)
        : identity_(std::move(identity)), entries_(capacity), freshnessNs_(freshnessNs) {
        validateConfig(capacity, freshnessNs);
        if (identity_.kind != MetricKind::Duration)
            throw std::invalid_argument("duration metric required");
    }
    [[nodiscard]] bool recordDuration(std::int64_t timeNs, std::uint64_t durationNs) noexcept {
        if (!identity_.supported) { ++rejected_; return false; }
        return insert({timeNs, durationNs, Outcome::Success});
    }
    [[nodiscard]] bool recordSeconds(std::int64_t timeNs, double seconds) noexcept {
        if (!std::isfinite(seconds) || seconds < 0 ||
            static_cast<long double>(seconds) * kOneSecondNs > static_cast<long double>(UINT64_MAX)) {
            ++rejected_;
            return false;
        }
        return recordDuration(timeNs, static_cast<std::uint64_t>(
            std::round(static_cast<long double>(seconds) * kOneSecondNs)));
    }
    [[nodiscard]] bool recordOutcome(std::int64_t timeNs, Outcome outcome) noexcept {
        if (outcome == Outcome::Success || outcome == Outcome::Count) {
            ++rejected_;
            return false;
        }
        if (!identity_.supported) { ++rejected_; return false; }
        return insert({timeNs, 0, outcome});
    }
    [[nodiscard]] DistributionSnapshot snapshot(std::int64_t nowNs, std::int64_t widthNs) const {
        if (widthNs <= 0 || nowNs < std::numeric_limits<std::int64_t>::min() + widthNs)
            throw std::invalid_argument("invalid statistics window");
        return snapshotBetween(nowNs - widthNs, nowNs, nowNs);
    }
    [[nodiscard]] DistributionSnapshot snapshotBetween(std::int64_t startNs,
                   std::int64_t endNs, std::int64_t nowNs) const {
        if (endNs < startNs || nowNs < endNs)
            throw std::invalid_argument("invalid experiment window");
        DistributionSnapshot out{};
        out.metric = identity_;
        out.windowStartNs = startNs;
        out.windowEndNs = endNs;
        out.rejectedInputs = rejected_;
        if (entries_.size()) out.lastObservationNs = entries_.back().time;
        out.availability = identity_.supported ?
            determineAvailability(out.lastObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_.at(i);
            if (e.time < startNs || e.time > endNs) continue;
            if (e.outcome == Outcome::Success) out.addDuration(e.ns);
            else out.addOutcome(e.outcome);
        }
        return out;
    }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return entries_.memoryBoundBytes(); }
private:
    bool insert(Entry entry) noexcept {
        if (entries_.size() && entry.time < entries_.back().time) {
            ++rejected_;
            return false;
        }
        entries_.push(entry);
        return true;
    }
    MetricIdentity identity_;
    Ring<Entry> entries_;
    std::int64_t freshnessNs_;
    std::uint64_t rejected_{};
};

struct FramePacingSnapshot {
    DistributionSnapshot intervals;
    std::uint64_t frameCount{};
    std::optional<double> fps;
    [[nodiscard]] std::optional<std::uint64_t> longestGapNs() const noexcept {
        return intervals.maxNs;
    }
};

class FramePacingWindow final {
public:
    explicit FramePacingWindow(MetricIdentity identity, std::size_t capacity = 16384,
                               std::int64_t freshnessNs = 2 * kOneSecondNs)
        : identity_(std::move(identity)), times_(capacity), freshnessNs_(freshnessNs) {
        validateConfig(capacity, freshnessNs);
        if (identity_.kind != MetricKind::FrameInterval)
            throw std::invalid_argument("frame interval metric required");
    }
    [[nodiscard]] bool recordFrame(std::int64_t timeNs) noexcept {
        if (!identity_.supported || (times_.size() && timeNs <= times_.back())) {
            ++rejected_; return false;
        }
        times_.push(timeNs);
        return true;
    }
    [[nodiscard]] FramePacingSnapshot snapshot(std::int64_t nowNs, std::int64_t widthNs) const {
        if (widthNs <= 0 || nowNs < std::numeric_limits<std::int64_t>::min() + widthNs)
            throw std::invalid_argument("invalid frame window");
        return snapshotBetween(nowNs - widthNs, nowNs, nowNs);
    }
    [[nodiscard]] FramePacingSnapshot snapshotBetween(std::int64_t startNs,
               std::int64_t endNs, std::int64_t nowNs) const {
        if (endNs < startNs || nowNs < endNs)
            throw std::invalid_argument("invalid frame experiment window");
        FramePacingSnapshot out{};
        auto& d = out.intervals;
        d.metric = identity_;
        d.windowStartNs = startNs;
        d.windowEndNs = endNs;
        d.rejectedInputs = rejected_;
        if (times_.size()) d.lastObservationNs = times_.back();
        d.availability = identity_.supported ?
            determineAvailability(d.lastObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        std::optional<std::int64_t> previous;
        for (std::size_t i = 0; i < times_.size(); ++i) {
            const auto t = times_.at(i);
            if (t < startNs || t > endNs) continue;
            ++out.frameCount;
            if (previous) d.addDuration(static_cast<std::uint64_t>(t) -
                                        static_cast<std::uint64_t>(*previous));
            previous = t;
        }
        if (d.sampleCount && d.sumNs > 0)
            out.fps = static_cast<double>(static_cast<long double>(d.sampleCount) *
                                          kOneSecondNs / d.sumNs);
        return out;
    }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return times_.memoryBoundBytes(); }
private:
    MetricIdentity identity_;
    Ring<std::int64_t> times_;
    std::int64_t freshnessNs_;
    std::uint64_t rejected_{};
};

struct CounterRateSnapshot {
    MetricIdentity metric;
    std::int64_t windowStartNs{};
    std::int64_t windowEndNs{};
    Availability availability{Availability::Unavailable};
    std::uint64_t observations{};
    std::uint64_t pairCount{};
    long double delta{};
    std::optional<double> ratePerSecond;
    std::uint64_t resetEvents{};
    std::uint64_t rejectedInputs{};
};

class CounterRateWindow final {
    struct Entry { std::int64_t time; std::uint64_t value; bool explicitWrap; };
public:
    explicit CounterRateWindow(MetricIdentity identity, std::size_t capacity = 16384,
                               std::int64_t freshnessNs = 2 * kOneSecondNs)
        : identity_(std::move(identity)), entries_(capacity), freshnessNs_(freshnessNs) {
        validateConfig(capacity, freshnessNs);
        if (identity_.kind != MetricKind::Counter)
            throw std::invalid_argument("counter metric required");
    }
    // Explicit generation transition or unmarked decrease starts a new rate segment.
    // explicitWrap must be provided by instrumented owner; never infer from decrease.
    [[nodiscard]] bool observe(std::int64_t timeNs, std::uint64_t value,
                               std::uint64_t generation, bool explicitWrap = false) noexcept {
        if (!identity_.supported) { ++rejected_; return false; }
        if (generation != identity_.resetGeneration) {
            identity_.resetGeneration = generation;
            entries_.clear();
            ++resets_;
        }
        if (entries_.size() && timeNs <= entries_.back().time) { ++rejected_; return false; }
        if (entries_.size() && value < entries_.back().value && !explicitWrap) {
            entries_.clear();
            ++resets_;
        }
        if (entries_.size() && explicitWrap && value >= entries_.back().value) {
            ++rejected_;
            return false;
        }
        if (entries_.size() == 0 && explicitWrap) { ++rejected_; return false; }
        entries_.push({timeNs, value, explicitWrap});
        return true;
    }
    [[nodiscard]] CounterRateSnapshot snapshot(std::int64_t nowNs, std::int64_t widthNs) const {
        if (widthNs <= 0 || nowNs < std::numeric_limits<std::int64_t>::min() + widthNs)
            throw std::invalid_argument("invalid counter window");
        return snapshotBetween(nowNs - widthNs, nowNs, nowNs);
    }
    [[nodiscard]] CounterRateSnapshot snapshotBetween(std::int64_t startNs,
                      std::int64_t endNs, std::int64_t nowNs) const {
        if (endNs < startNs || nowNs < endNs)
            throw std::invalid_argument("invalid counter experiment window");
        CounterRateSnapshot out{};
        out.metric = identity_;
        out.windowStartNs = startNs;
        out.windowEndNs = endNs;
        out.resetEvents = resets_;
        out.rejectedInputs = rejected_;
        if (!identity_.supported) out.availability = Availability::Unsupported;
        else if (entries_.size())
            out.availability = determineAvailability(entries_.back().time, nowNs, freshnessNs_);
        std::optional<Entry> previous;
        std::int64_t first{};
        std::int64_t last{};
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_.at(i);
            if (e.time < startNs || e.time > endNs) continue;
            ++out.observations;
            if (!previous) first = e.time;
            else {
                ++out.pairCount;
                const std::uint64_t delta = e.explicitWrap ?
                    (UINT64_MAX - previous->value) + 1 + e.value : e.value - previous->value;
                out.delta += static_cast<long double>(delta);
            }
            previous = e;
            last = e.time;
        }
        if (out.pairCount && last > first)
            out.ratePerSecond = static_cast<double>(out.delta * kOneSecondNs /
                                                    (static_cast<long double>(last) - static_cast<long double>(first)));
        return out;
    }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return entries_.memoryBoundBytes(); }
private:
    MetricIdentity identity_;
    Ring<Entry> entries_;
    std::int64_t freshnessNs_;
    std::uint64_t resets_{};
    std::uint64_t rejected_{};
};

} // namespace reg::diagnostics::stats
