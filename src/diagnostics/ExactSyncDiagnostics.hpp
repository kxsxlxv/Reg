#pragma once

#include "media/FrameIdentity.hpp"
#include "metadata/FrameMetadata.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace reg::diagnostics {

// Reg steady_clock only. No source/Jetson monotonic clock may be subtracted.
// Every counter resets on resetSession(); sessionGeneration distinguishes resets.
// matchCoverage is presentedExactTotal / decidedFramesTotal, NOT a prediction
// for pending frames. No denominator (zero decisions) => nullopt.
struct ExactSyncSnapshot {
    std::optional<media::FrameKey> lastVideoKey;
    std::optional<media::FrameKey> lastMetadataKey;
    std::optional<media::FrameKey> lastMatchedKey;
    std::uint32_t lastMatchedTargetCount{};
    // Backward-compatible counters used by existing telemetry.
    std::uint64_t matchedPairs{};
    std::uint64_t rejectedKeyMismatches{};
    std::uint64_t dueWithoutMetadata{};
    std::uint64_t metadataAfterDroppedFrame{};
    std::uint64_t metadataArrivedAfterDeadline{};
    std::uint64_t matchedAfterDeadline{};
    double lastArrivalRelativeToDecodeMs{};
    double lastPlayoutWaitMs{};
    double arrivalDelayP50Ms{};
    double arrivalDelayP95Ms{};
    double arrivalDelayP99Ms{};
    std::size_t latencySampleCount{};
    bool hasTimingSample{};

    // New lifecycle counters. Units: frames, packets or occurrences as named.
    std::uint64_t sessionGeneration{};
    std::uint64_t decodedVideoTotal{};
    std::uint64_t duplicateVideoKeysTotal{};
    std::uint64_t nonmonotonicVideoKeysTotal{};
    std::uint64_t videoEpochTransitionsTotal{};
    std::uint64_t eligibleFramesTotal{};
    std::uint64_t presentedExactTotal{};
    std::uint64_t dropMissingTotal{};
    std::uint64_t evictedTotal{};
    std::uint64_t missingIdentityTotal{};
    std::uint64_t duplicateLifecycleKeysTotal{};
    std::uint64_t conflictingDecisionsTotal{};
    std::uint64_t metadataObservedTotal{}; // distinct accepted FrameKeys in retention horizon
    std::uint64_t metadataDuplicateTotal{};
    std::uint64_t metadataBeforeDecodeTotal{};
    std::uint64_t metadataWithinDeadlineTotal{};
    std::uint64_t metadataAfterDeadlineBeforeDecisionTotal{};
    std::uint64_t metadataLateObservedTotal{}; // after deadline, including after drop
    std::uint64_t metadataOtherEpochTotal{}; // observational, not a reject verdict
    std::uint64_t validEmptyDetectionsTotal{};
    // Censored observations, NOT UDP loss or proof CV never ran.
    std::uint64_t metadataUnobservedAtExpiryTotal{};
    std::uint64_t unknownAtExpiryTotal{};
    std::uint64_t unknownAtClearTotal{};
    std::uint64_t capacityCensoredTotal{}; // pending frames evicted from history
    std::uint64_t historyForcedRetirementTotal{}; // all history capacity retirements
    std::uint64_t unmatchedMetadataExpiredTotal{};
    std::size_t retainedLifecycleCount{};
    std::size_t pendingFrameCount{};
    std::size_t orphanMetadataCount{};
    std::uint64_t decidedFramesTotal{};
    std::optional<double> matchCoverage;
    // The legacy percentile sample is the last <=256 matched pairs ONLY;
    // values are max(0, Reg metadata receipt - Reg decode), milliseconds.
    // Counters have process-session cumulative population; percentile last-N
    // is a separate, biased population. No total CV latency is calculated.
};

class ExactSyncDiagnostics final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr std::size_t kHistoryCapacity = 256U;
    static constexpr auto kRetentionAfterDeadline = std::chrono::seconds{5};

    void videoDecoded(media::FrameKey key) {
        std::scoped_lock lock(mutex_);
        ++state_.decodedVideoTotal;
        if (state_.lastVideoKey) {
            const auto old = *state_.lastVideoKey;
            if (key == old) {
                ++state_.duplicateVideoKeysTotal;
            } else if (key.streamEpoch == old.streamEpoch && key.frameId < old.frameId) {
                ++state_.nonmonotonicVideoKeysTotal;
            }
            if (key.streamEpoch != old.streamEpoch) {
                ++state_.videoEpochTransitionsTotal;
            }
        }
        state_.lastVideoKey = key;
    }

    // Call only on a successful OverlayFrameBuffer admission, not for Raw.
    // The decodedAt/deadline points must share Reg's steady_clock domain.
    void overlayBuffered(media::FrameKey key, TimePoint decodedAt, TimePoint deadline) {
        std::scoped_lock lock(mutex_);
        auto& entry = getOrCreate(key);
        if (entry.eligible) {
            ++state_.duplicateLifecycleKeysTotal;
            return;
        }
        entry.eligible = true;
        entry.decodedAt = decodedAt;
        entry.deadline = deadline;
        ++state_.eligibleFramesTotal;
        classifyMetadata(entry);
    }

    void missingOverlayIdentity() {
        std::scoped_lock lock(mutex_);
        ++state_.missingIdentityTotal;
    }

    void metadataAccepted(const metadata::FrameMetadata& value) {
        std::scoped_lock lock(mutex_);
        state_.lastMetadataKey = value.key;
        auto& entry = getOrCreate(value.key);
        if (entry.metadataAt) {
            ++state_.metadataDuplicateTotal;
            return;
        }
        entry.metadataAt = value.receivedAt;
        if (entry.decision == Decision::Missing || entry.decision == Decision::Evicted) {
            entry.metadataArrivedAfterDrop = true;
            ++state_.metadataAfterDroppedFrame;
            if (entry.deadline && value.receivedAt > *entry.deadline) {
                ++state_.metadataArrivedAfterDeadline;
            }
        }
        ++state_.metadataObservedTotal;
        if (value.targets.empty()) {
            ++state_.validEmptyDetectionsTotal;
        }
        if (state_.lastVideoKey &&
            state_.lastVideoKey->streamEpoch != value.key.streamEpoch) {
            ++state_.metadataOtherEpochTotal;
        }
        classifyMetadata(entry);
    }

    // A due/drop decision may be observed without an earlier admission hook
    // while integrating the new instrumentation; infer one eligible frame.
    void frameDueWithoutMetadata(media::FrameKey key, TimePoint deadline) {
        std::scoped_lock lock(mutex_);
        auto& entry = getOrCreate(key);
        if (entry.decision != Decision::None) {
            if (entry.decision != Decision::Missing) ++state_.conflictingDecisionsTotal;
            return;
        }
        admitAtDecision(entry, deadline);
        entry.decision = Decision::Missing;
        ++state_.dueWithoutMetadata;
        ++state_.dropMissingTotal;
    }

    // Called with the evicted frame identity, not the newly pushed frame.
    // OverlayFrameBuffer::push presently reports only EvictedOldest, without
    // the evicted key. Coordinator must expose that identity to wire this.
    void frameBufferEvicted(media::FrameKey key, TimePoint deadline) {
        std::scoped_lock lock(mutex_);
        auto& entry = getOrCreate(key);
        if (entry.decision != Decision::None) {
            if (entry.decision != Decision::Evicted) ++state_.conflictingDecisionsTotal;
            return;
        }
        admitAtDecision(entry, deadline);
        entry.decision = Decision::Evicted;
        ++state_.evictedTotal;
    }

    // Identity validation is unchanged: never pair neighboring frame IDs.
    // Return value remains the exact-key test, independent of diagnostics.
    bool exactPair(media::FrameKey videoKey, const metadata::FrameMetadata& value,
                   TimePoint decodedAt, TimePoint deadline, TimePoint matchedAt) {
        std::scoped_lock lock(mutex_);
        if (videoKey != value.key) {
            ++state_.rejectedKeyMismatches;
            return false;
        }
        auto& entry = getOrCreate(videoKey);
        admitAtDecision(entry, deadline);
        if (!entry.decodedAt) entry.decodedAt = decodedAt;
        if (!entry.metadataAt) {
            entry.metadataAt = value.receivedAt;
            ++state_.metadataObservedTotal;
            if (value.targets.empty()) ++state_.validEmptyDetectionsTotal;
        }
        classifyMetadata(entry);
        if (entry.decision != Decision::None) {
            if (entry.decision != Decision::Presented) ++state_.conflictingDecisionsTotal;
            return true;
        }
        entry.decision = Decision::Presented;
        ++state_.matchedPairs;
        ++state_.presentedExactTotal;
        state_.lastMatchedKey = videoKey;
        state_.lastMatchedTargetCount = static_cast<std::uint32_t>(value.targets.size());
        state_.lastArrivalRelativeToDecodeMs =
            std::chrono::duration<double, std::milli>(value.receivedAt - decodedAt).count();
        state_.lastPlayoutWaitMs =
            std::chrono::duration<double, std::milli>(matchedAt - decodedAt).count();
        state_.hasTimingSample = true;
        if (value.receivedAt > deadline) ++state_.matchedAfterDeadline;
        if (arrivalWaitMs_.size() == kHistoryCapacity) arrivalWaitMs_.pop_front();
        arrivalWaitMs_.push_back(std::max(0.0, state_.lastArrivalRelativeToDecodeMs));
        return true;
    }

    // Invoke on a low-priority diagnostic thread with virtual/Reg time.
    // Frames retained past deadline + 5 seconds are right-censored. Capacity
    // retirements before this boundary are separately counted and NOT called
    // metadata unobserved at expiry. Events beyond the 256-key horizon cannot
    // be correlated; no claim that a packet was lost on the network.
    // Mutable Overlay deadline changes require a future coordinator callback
    // and classification-revision policy; no such runtime hook exists yet.
    // Unknown pending frames are right-censored, never silently marked drops.
    void expire(TimePoint now) {
        std::scoped_lock lock(mutex_);
        for (auto it = history_.begin(); it != history_.end();) {
            const auto boundary = it->deadline.value_or(it->metadataAt.value_or(now));
            if (now < boundary || now - boundary < kRetentionAfterDeadline) {
                ++it;
                continue;
            }
            if (it->eligible) {
                if (it->decision == Decision::None) ++state_.unknownAtExpiryTotal;
                else if (it->decision != Decision::Presented && !it->metadataAt) {
                    ++state_.metadataUnobservedAtExpiryTotal;
                }
            } else if (it->metadataAt) {
                ++state_.unmatchedMetadataExpiredTotal;
            }
            it = history_.erase(it);
        }
    }

    // Runtime flush: pending outcomes become UNKNOWN, never missing/lost.
    // Does not reset cumulative counters or the last-N matched-only samples.
    void clearPending() {
        std::scoped_lock lock(mutex_);
        for (const auto& item : history_) {
            if (item.eligible && item.decision == Decision::None) {
                ++state_.unknownAtClearTotal;
            }
        }
        history_.clear();
    }

    // Process/decoder/session reset; do not compare counter deltas across it.
    void resetSession() {
        std::scoped_lock lock(mutex_);
        const auto generation = state_.sessionGeneration + 1U;
        state_ = {};
        state_.sessionGeneration = generation;
        history_.clear();
        arrivalWaitMs_.clear();
    }

    ExactSyncSnapshot snapshot() const {
        std::vector<double> sorted;
        ExactSyncSnapshot result;
        {
            std::scoped_lock lock(mutex_);
            result = state_;
            result.retainedLifecycleCount = history_.size();
            for (const auto& entry : history_) {
                if (entry.eligible && entry.decision == Decision::None) {
                    ++result.pendingFrameCount;
                } else if (!entry.eligible && entry.metadataAt) {
                    ++result.orphanMetadataCount;
                }
            }
            sorted.assign(arrivalWaitMs_.begin(), arrivalWaitMs_.end());
        }
        result.decidedFramesTotal = result.presentedExactTotal +
            result.dropMissingTotal + result.evictedTotal;
        if (result.decidedFramesTotal != 0U) {
            result.matchCoverage = static_cast<double>(result.presentedExactTotal) /
                static_cast<double>(result.decidedFramesTotal);
        }
        if (!sorted.empty()) {
            std::sort(sorted.begin(), sorted.end()); // off hot path, outside mutex
            result.latencySampleCount = sorted.size();
            result.arrivalDelayP50Ms = percentile(sorted, 50U);
            result.arrivalDelayP95Ms = percentile(sorted, 95U);
            result.arrivalDelayP99Ms = percentile(sorted, 99U);
        }
        return result;
    }

private:
    enum class Decision { None, Presented, Missing, Evicted };
    struct LifecycleEntry {
        media::FrameKey key{};
        std::optional<TimePoint> decodedAt;
        std::optional<TimePoint> deadline;
        std::optional<TimePoint> metadataAt;
        Decision decision{Decision::None};
        bool eligible{};
        bool classified{};
        bool lateRecorded{};
        bool metadataArrivedAfterDrop{};
    };

    LifecycleEntry& getOrCreate(media::FrameKey key) {
        const auto found = std::find_if(history_.begin(), history_.end(),
            [&](const auto& entry) { return entry.key == key; });
        if (found != history_.end()) return *found;
        if (history_.size() == kHistoryCapacity) {
            ++state_.historyForcedRetirementTotal;
            const auto& victim = history_.front();
            if (victim.eligible && victim.decision == Decision::None) {
                ++state_.capacityCensoredTotal;
            }
            history_.pop_front();
        }
        LifecycleEntry item{};
        item.key = key;
        history_.push_back(item);
        return history_.back();
    }

    void admitAtDecision(LifecycleEntry& entry, TimePoint deadline) {
        if (!entry.eligible) {
            entry.eligible = true;
            ++state_.eligibleFramesTotal;
        }
        if (!entry.deadline) entry.deadline = deadline;
        classifyMetadata(entry);
    }

    void classifyMetadata(LifecycleEntry& entry) {
        if (!entry.eligible || !entry.metadataAt || !entry.deadline) return;
        const bool late = *entry.metadataAt > *entry.deadline;
        if (late && !entry.lateRecorded) {
            ++state_.metadataLateObservedTotal;
            entry.lateRecorded = true;
        }
        if (entry.classified || !entry.decodedAt) return;
        entry.classified = true;
        if (*entry.metadataAt < *entry.decodedAt) {
            ++state_.metadataBeforeDecodeTotal;
        } else if (!late) {
            ++state_.metadataWithinDeadlineTotal;
        } else if (!entry.metadataArrivedAfterDrop) {
            ++state_.metadataAfterDeadlineBeforeDecisionTotal;
        }
    }

    static double percentile(const std::vector<double>& sorted, std::size_t percent) {
        return sorted[((sorted.size() - 1U) * percent + 99U) / 100U];
    }

    mutable std::mutex mutex_;
    ExactSyncSnapshot state_{};
    std::deque<LifecycleEntry> history_;
    std::deque<double> arrivalWaitMs_;
};

} // namespace reg::diagnostics
