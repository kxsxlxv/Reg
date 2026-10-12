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
// Same-window shards cannot be deduplicated from their histogram alone.
// Caller must independently establish non-overlapping sample identities.
enum class Disjointness { Unverified, VerifiedDisjoint };

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
    std::uint64_t inferredResetSequence{}; // increments after an unmarked counter decrease
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
    Availability availability{Availability::Unavailable}; // most recent IN-WINDOW sample, measured at query time
    Availability sourceAvailability{Availability::Unavailable}; // latest source sample, measured at query time
    std::int64_t queriedAtNs{};
    std::int64_t freshnessThresholdNs{};
    std::optional<std::int64_t> lastObservationNs; // in requested window only
    std::optional<std::int64_t> lastSourceObservationNs; // stream-wide
    std::optional<std::int64_t> firstEvictedNs;
    std::optional<std::int64_t> lastEvictedNs;
    bool historyTruncated{}; // at least one observation evicted from this generation
    bool windowComplete{true}; // no known evicted observations within window bounds
    std::optional<std::uint64_t> rejectedInputs; // lifetime diagnostic, NOT additive/mergeable
    bool percentilesAreUpperBoundEstimates{true};
    double quantileRelativeUpperErrorBound{0.0625};
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

    [[nodiscard]] std::optional<long double> meanNs(bool allowPartial = false) const noexcept {
        if (!sampleCount || (!windowComplete && !allowPartial)) return std::nullopt;
        return sumNs / static_cast<long double>(sampleCount);
    }

    [[nodiscard]] std::optional<double> coverage(bool allowPartial = false) const noexcept {
        const auto n = observedCount();
        if (!n || (!windowComplete && !allowPartial)) return std::nullopt;
        return static_cast<double>(sampleCount) / static_cast<double>(n);
    }

    // Nearest-rank percentile; conservative upper endpoint of logarithmic bin.
    // For 1-s windows P99 with ~60 samples is descriptive, not a stable tail.
    [[nodiscard]] std::optional<std::uint64_t> percentileUpper(
        double percentile, bool allowPartial = false) const noexcept {
        if (!sampleCount || (!windowComplete && !allowPartial) ||
            !std::isfinite(percentile) || percentile < 0 || percentile > 100)
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

    // Same-interval histogram shards have no per-sample IDs for deduplication.
    // Explicit disjointness evidence is required. Incomplete inputs are refused
    // unless the caller explicitly requests an incomplete merged view.
    [[nodiscard]] std::optional<DistributionSnapshot> mergedWith(
        const DistributionSnapshot& other,
        Disjointness evidence = Disjointness::Unverified,
        bool allowPartial = false) const {
        if (evidence != Disjointness::VerifiedDisjoint ||
            windowStartNs != other.windowStartNs ||
            windowEndNs != other.windowEndNs) return std::nullopt;
        return mergeCompatible(other, allowPartial);
    }

    // Inclusive nanosecond bounds: [0, 9] and [10, 19] are adjacent;
    // [0, 9] with [9, 19] overlaps and is rejected without deduplication.
    // Unlike same-interval shard merging, ordered adjacent intervals are
    // provably time-disjoint and need no external identity attestation.
    [[nodiscard]] std::optional<DistributionSnapshot> mergedAdjacentWith(
        const DistributionSnapshot& later, bool allowPartial = false) const {
        if (windowEndNs == std::numeric_limits<std::int64_t>::max() ||
            windowEndNs + 1 != later.windowStartNs ||
            later.windowEndNs < later.windowStartNs) return std::nullopt;
        auto combined = mergeCompatible(later, allowPartial);
        if (combined) combined->windowEndNs = later.windowEndNs;
        return combined;
    }

private:
    [[nodiscard]] std::optional<DistributionSnapshot> mergeCompatible(
        const DistributionSnapshot& other, bool allowPartial) const {
        if (!(metric == other.metric) || queriedAtNs != other.queriedAtNs ||
            freshnessThresholdNs != other.freshnessThresholdNs ||
            (!allowPartial && (!windowComplete || !other.windowComplete)) ||
            availability == Availability::Unsupported ||
            other.availability == Availability::Unsupported ||
            sampleCount > UINT64_MAX - other.sampleCount) return std::nullopt;
        for (std::size_t i = 0; i < kHistogramBuckets; ++i)
            if (histogram[i] > UINT64_MAX - other.histogram[i]) return std::nullopt;
        for (std::size_t i = 0; i < kOutcomeCount; ++i)
            if (outcomes[i] > UINT64_MAX - other.outcomes[i]) return std::nullopt;
        DistributionSnapshot merged = *this;
        merged.sampleCount += other.sampleCount;
        merged.sumNs += other.sumNs;
        merged.rejectedInputs = std::nullopt; // unmergeable lifetime diagnostic
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
        if (other.lastSourceObservationNs &&
            (!merged.lastSourceObservationNs ||
             *other.lastSourceObservationNs > *merged.lastSourceObservationNs))
            merged.lastSourceObservationNs = other.lastSourceObservationNs;
        if (other.firstEvictedNs &&
            (!merged.firstEvictedNs || *other.firstEvictedNs < *merged.firstEvictedNs))
            merged.firstEvictedNs = other.firstEvictedNs;
        if (other.lastEvictedNs &&
            (!merged.lastEvictedNs || *other.lastEvictedNs > *merged.lastEvictedNs))
            merged.lastEvictedNs = other.lastEvictedNs;
        merged.historyTruncated = historyTruncated || other.historyTruncated;
        merged.windowComplete = windowComplete && other.windowComplete;
        auto classify = [&](std::optional<std::int64_t> t) {
            if (!t || *t > queriedAtNs) return Availability::Unavailable;
            return static_cast<std::uint64_t>(queriedAtNs) -
                   static_cast<std::uint64_t>(*t) >
                   static_cast<std::uint64_t>(freshnessThresholdNs)
                       ? Availability::Stale : Availability::Fresh;
        };
        merged.availability = classify(merged.lastObservationNs);
        merged.sourceAvailability = classify(merged.lastSourceObservationNs);
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
    [[nodiscard]] const T& front() const noexcept { return at(0); }
    [[nodiscard]] std::size_t capacity() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return data_.size() * sizeof(T); }
private:
    std::vector<T> data_;
    std::size_t head_{};
    std::size_t size_{};
};

inline bool evictedHistoryIntersects(std::optional<std::int64_t> firstEvicted,
                                     std::optional<std::int64_t> lastEvicted,
                                     std::int64_t start, std::int64_t end) noexcept {
    return firstEvicted && lastEvicted && *firstEvicted <= end && *lastEvicted >= start;
}

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
        out.queriedAtNs = nowNs;
        out.freshnessThresholdNs = freshnessNs_;
        out.firstEvictedNs = firstEvictedNs_;
        out.lastEvictedNs = lastEvictedNs_;
        out.historyTruncated = firstEvictedNs_.has_value();
        out.windowComplete = !evictedHistoryIntersects(firstEvictedNs_, lastEvictedNs_,
                                                        startNs, endNs);
        if (entries_.size()) out.lastSourceObservationNs = entries_.back().time;
        out.sourceAvailability = identity_.supported ?
            determineAvailability(out.lastSourceObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_.at(i);
            if (e.time < startNs || e.time > endNs) continue;
            out.lastObservationNs = e.time;
            if (e.outcome == Outcome::Success) out.addDuration(e.ns);
            else out.addOutcome(e.outcome);
        }
        out.availability = identity_.supported ?
            determineAvailability(out.lastObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        return out;
    }
    [[nodiscard]] std::size_t memoryBoundBytes() const noexcept { return entries_.memoryBoundBytes(); }
private:
    bool insert(Entry entry) noexcept {
        if (entry.time < 0 || (entries_.size() && entry.time < entries_.back().time)) {
            ++rejected_;
            return false;
        }
        if (entries_.size() == entries_.capacity()) {
            const auto evicted = entries_.front().time;
            if (!firstEvictedNs_) firstEvictedNs_ = evicted;
            lastEvictedNs_ = evicted;
        }
        entries_.push(entry);
        return true;
    }
    MetricIdentity identity_;
    Ring<Entry> entries_;
    std::int64_t freshnessNs_;
    std::uint64_t rejected_{};
    std::optional<std::int64_t> firstEvictedNs_, lastEvictedNs_;
};

struct FramePacingSnapshot {
    DistributionSnapshot intervals;
    std::uint64_t frameCount{};
    std::optional<double> fps; // unavailable for known-truncated window
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
        if (!identity_.supported || timeNs < 0 ||
            (times_.size() && timeNs <= times_.back())) {
            ++rejected_; return false;
        }
        if (times_.size() == times_.capacity()) {
            const auto evicted = times_.front();
            if (!firstEvictedNs_) firstEvictedNs_ = evicted;
            lastEvictedNs_ = evicted;
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
        d.queriedAtNs = nowNs;
        d.freshnessThresholdNs = freshnessNs_;
        d.firstEvictedNs = firstEvictedNs_;
        d.lastEvictedNs = lastEvictedNs_;
        d.historyTruncated = firstEvictedNs_.has_value();
        d.windowComplete = !evictedHistoryIntersects(firstEvictedNs_, lastEvictedNs_,
                                                      startNs, endNs);
        if (times_.size()) d.lastSourceObservationNs = times_.back();
        d.sourceAvailability = identity_.supported ?
            determineAvailability(d.lastSourceObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        std::optional<std::int64_t> previous;
        for (std::size_t i = 0; i < times_.size(); ++i) {
            const auto t = times_.at(i);
            if (t < startNs || t > endNs) continue;
            d.lastObservationNs = t;
            ++out.frameCount;
            if (previous) d.addDuration(static_cast<std::uint64_t>(t) -
                                        static_cast<std::uint64_t>(*previous));
            previous = t;
        }
        d.availability = identity_.supported ?
            determineAvailability(d.lastObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        if (d.windowComplete && d.sampleCount && d.sumNs > 0)
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
    std::optional<std::int64_t> firstEvictedNs_, lastEvictedNs_;
};

struct CounterRateSnapshot {
    MetricIdentity metric;
    std::int64_t windowStartNs{};
    std::int64_t windowEndNs{};
    Availability availability{Availability::Unavailable};
    Availability sourceAvailability{Availability::Unavailable};
    std::optional<std::int64_t> lastObservationNs;
    std::optional<std::int64_t> lastSourceObservationNs;
    std::optional<std::int64_t> firstEvictedNs, lastEvictedNs;
    bool historyTruncated{};
    bool windowComplete{true};
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
        if (!identity_.supported || timeNs < 0) { ++rejected_; return false; }
        if (generation != identity_.resetGeneration) {
            identity_.resetGeneration = generation;
            identity_.inferredResetSequence = 0;
            entries_.clear();
            firstEvictedNs_.reset();
            lastEvictedNs_.reset();
            lastResetAtNs_ = timeNs;
            ++resets_;
        }
        if (entries_.size() && timeNs <= entries_.back().time) { ++rejected_; return false; }
        if (entries_.size() && value < entries_.back().value && !explicitWrap) {
            entries_.clear();
            firstEvictedNs_.reset();
            lastEvictedNs_.reset();
            lastResetAtNs_ = timeNs;
            ++identity_.inferredResetSequence;
            ++resets_;
        }
        if (entries_.size() && explicitWrap && value >= entries_.back().value) {
            ++rejected_;
            return false;
        }
        if (entries_.size() == 0 && explicitWrap) { ++rejected_; return false; }
        if (entries_.size() == entries_.capacity()) {
            const auto evicted = entries_.front().time;
            if (!firstEvictedNs_) firstEvictedNs_ = evicted;
            lastEvictedNs_ = evicted;
        }
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
        out.firstEvictedNs = firstEvictedNs_;
        out.lastEvictedNs = lastEvictedNs_;
        out.historyTruncated = firstEvictedNs_.has_value();
        out.windowComplete = !evictedHistoryIntersects(firstEvictedNs_, lastEvictedNs_,
                                                       startNs, endNs) &&
                             !(lastResetAtNs_ && startNs < *lastResetAtNs_ &&
                               *lastResetAtNs_ <= endNs);
        if (entries_.size()) out.lastSourceObservationNs = entries_.back().time;
        out.sourceAvailability = identity_.supported ?
            determineAvailability(out.lastSourceObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        std::optional<Entry> previous;
        std::int64_t first{};
        std::int64_t last{};
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_.at(i);
            if (e.time < startNs || e.time > endNs) continue;
            out.lastObservationNs = e.time;
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
        out.availability = identity_.supported ?
            determineAvailability(out.lastObservationNs, nowNs, freshnessNs_) : Availability::Unsupported;
        if (out.windowComplete && out.pairCount && last > first)
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
    std::optional<std::int64_t> firstEvictedNs_, lastEvictedNs_, lastResetAtNs_;
};

} // namespace reg::diagnostics::stats
