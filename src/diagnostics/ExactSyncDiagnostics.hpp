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

// Local-clock statistics only: Jetson cvBeginNs/cvEndNs are not comparable
// to the Windows steady clock without explicit clock synchronization.
struct ExactSyncSnapshot {
    std::optional<media::FrameKey> lastVideoKey;
    std::optional<media::FrameKey> lastMetadataKey;
    std::optional<media::FrameKey> lastMatchedKey;
    std::uint32_t lastMatchedTargetCount{};
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
};

class ExactSyncDiagnostics final {
public:
    void videoDecoded(media::FrameKey key) {
        std::scoped_lock lock(mutex_);
        snapshot_.lastVideoKey = key;
    }

    void metadataAccepted(const metadata::FrameMetadata& value) {
        std::scoped_lock lock(mutex_);
        snapshot_.lastMetadataKey = value.key;

        // Only compare against the exact identity of a frame that Reg
        // already discarded. Arrival order and neighboring IDs are irrelevant.
        const auto it = std::find_if(
            dropped_.begin(), dropped_.end(),
            [&](const DroppedFrame& dropped) {
                return dropped.key == value.key;
            });
        if (it != dropped_.end()) {
            ++snapshot_.metadataAfterDroppedFrame;
            if (value.receivedAt > it->deadline) {
                ++snapshot_.metadataArrivedAfterDeadline;
            }
            dropped_.erase(it);
        }
    }

    void frameDueWithoutMetadata(
        media::FrameKey key,
        std::chrono::steady_clock::time_point deadline) {
        std::scoped_lock lock(mutex_);
        ++snapshot_.dueWithoutMetadata;
        // A bounded diagnostic history, not a second metadata queue.
        if (dropped_.size() == kHistoryCapacity) {
            dropped_.pop_front();
        }
        dropped_.push_back(DroppedFrame{key, deadline});
    }

    // Return false rather than allowing a wrong-frame overlay through.
    bool exactPair(
        media::FrameKey videoKey,
        const metadata::FrameMetadata& value,
        std::chrono::steady_clock::time_point decodedAt,
        std::chrono::steady_clock::time_point deadline,
        std::chrono::steady_clock::time_point matchedAt) {
        std::scoped_lock lock(mutex_);
        if (videoKey != value.key) {
            ++snapshot_.rejectedKeyMismatches;
            return false;
        }

        ++snapshot_.matchedPairs;
        snapshot_.lastMatchedKey = videoKey;
        snapshot_.lastMatchedTargetCount =
            static_cast<std::uint32_t>(value.targets.size());
        snapshot_.lastArrivalRelativeToDecodeMs =
            std::chrono::duration<double, std::milli>(
                value.receivedAt - decodedAt).count();
        snapshot_.lastPlayoutWaitMs =
            std::chrono::duration<double, std::milli>(
                matchedAt - decodedAt).count();
        snapshot_.hasTimingSample = true;

        if (value.receivedAt > deadline) {
            ++snapshot_.matchedAfterDeadline;
        }

        // Treat CVM1 packets received before local decoding as zero
        // additional waiting; never interpret this as negative latency.
        const double waitMs = std::max(
            0.0, snapshot_.lastArrivalRelativeToDecodeMs);
        if (arrivalWaitMs_.size() == kHistoryCapacity) {
            arrivalWaitMs_.pop_front();
        }
        arrivalWaitMs_.push_back(waitMs);
        return true;
    }

    void clearPending() {
        std::scoped_lock lock(mutex_);
        dropped_.clear();
    }

    ExactSyncSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        ExactSyncSnapshot result = snapshot_;
        if (!arrivalWaitMs_.empty()) {
            std::vector<double> values(
                arrivalWaitMs_.begin(), arrivalWaitMs_.end());
            std::sort(values.begin(), values.end());
            result.latencySampleCount = values.size();
            result.arrivalDelayP50Ms = percentile(values, 50U);
            result.arrivalDelayP95Ms = percentile(values, 95U);
            result.arrivalDelayP99Ms = percentile(values, 99U);
        }
        return result;
    }

private:
    struct DroppedFrame {
        media::FrameKey key{};
        std::chrono::steady_clock::time_point deadline{};
    };

    static constexpr std::size_t kHistoryCapacity = 256U;

    static double percentile(
        const std::vector<double>& sorted, std::size_t percent) {
        const std::size_t index =
            ((sorted.size() - 1U) * percent + 99U) / 100U;
        return sorted[index];
    }

    mutable std::mutex mutex_;
    ExactSyncSnapshot snapshot_{};
    std::deque<DroppedFrame> dropped_;
    std::deque<double> arrivalWaitMs_;
};

} // namespace reg::diagnostics
