// Offline bridge: processes the synthetic Reg-side frame lifecycle through the
// real C++ ExactSyncDiagnostics and Statistics implementations. No RTSP/GPU.
#include "diagnostics/ExactSyncDiagnostics.hpp"
#include "diagnostics/stats/Statistics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using reg::media::FrameKey;
using reg::metadata::FrameMetadata;
using reg::diagnostics::ExactSyncDiagnostics;
namespace stats = reg::diagnostics::stats;
using Nanoseconds = std::chrono::nanoseconds;
using TimePoint = std::chrono::steady_clock::time_point;

struct FrameRecord {
    std::optional<TimePoint> decoded;
    std::optional<TimePoint> deadline;
    std::optional<FrameMetadata> metadata;
};

TimePoint timePoint(std::int64_t ns) {
    if (ns < 0) throw std::runtime_error("negative Reg clock");
    return TimePoint{Nanoseconds{ns}};
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}
}

int main() {
    try {
        ExactSyncDiagnostics sync;
        stats::DurationWindow cvWaiting(
            stats::MetricIdentity{"reg.cv_wait", "ns", "windows-reg",
                                  "overlay_decisions", "reg-steady", "fixture",
                                  0, stats::MetricKind::Duration}, 4096);
        stats::FramePacingWindow decoderPacing(
            stats::MetricIdentity{"reg.decoded_frame_gap", "ns", "windows-reg",
                                  "decoded_frame_pairs", "reg-steady", "fixture",
                                  0, stats::MetricKind::FrameInterval}, 4096);
        std::map<FrameKey, FrameRecord> records;
        std::string line;
        std::uint64_t observedDecode = 0;
        std::uint64_t rejectedPacing = 0;
        std::uint64_t observedDecisions = 0;
        std::int64_t lastTime = 0;

        // Wire protocol: one text row per *Reg-local* synthetic observation,
        // already ordered by virtual time. No Jetson timestamps are exposed:
        // D/M/P/X/E epoch frame localRegNs deadlineRegNs targets.
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            std::istringstream row(line);
            char kind{};
            std::uint64_t epoch{}, frame{}, count{};
            std::int64_t t{}, deadlineNs{};
            if (!(row >> kind >> epoch >> frame >> t >> deadlineNs >> count)) {
                fail("malformed fixture projection row");
            }
            std::string extra;
            if (row >> extra || t < 0 || deadlineNs < 0 || t < lastTime) {
                fail("unexpected extra fields / nonmonotonic local Reg time");
            }
            lastTime = t;
            const FrameKey key{epoch, frame};
            auto& record = records[key];
            const TimePoint time = timePoint(t);
            if (kind == 'D') {
                if (record.decoded) fail("duplicate video decode identity");
                record.decoded = time;
                record.deadline = timePoint(deadlineNs);
                sync.videoDecoded(key);
                sync.overlayBuffered(key, time, *record.deadline);
                ++observedDecode;
                if (!decoderPacing.recordFrame(t)) ++rejectedPacing;
            } else if (kind == 'M') {
                FrameMetadata meta{};
                meta.key = key;
                meta.receivedAt = time;
                meta.targets.resize(static_cast<std::size_t>(count));
                sync.metadataAccepted(meta);
                if (!record.metadata) record.metadata = meta;
            } else if (kind == 'P') {
                if (!record.decoded || !record.deadline || !record.metadata ||
                    record.metadata->receivedAt > time) fail("match without exact timely metadata");
                if (!sync.exactPair(key, *record.metadata, *record.decoded,
                                    *record.deadline, time)) {
                    fail("exact sync identity rejection on synthetic match");
                }
                const auto signedWait = std::chrono::duration_cast<Nanoseconds>(
                    record.metadata->receivedAt - *record.decoded).count();
                if (!cvWaiting.recordDuration(t, static_cast<std::uint64_t>(
                        std::max<std::int64_t>(0, signedWait)))) {
                    fail("statistics rejected matched wait");
                }
                ++observedDecisions;
            } else if (kind == 'X') {
                if (!record.decoded || !record.deadline) fail("drop without decoded frame");
                sync.frameDueWithoutMetadata(key, *record.deadline);
                if (!cvWaiting.recordOutcome(t, stats::Outcome::Missing)) {
                    fail("statistics rejected missing outcome");
                }
                ++observedDecisions;
            } else if (kind == 'E') {
                if (!record.decoded || !record.deadline) fail("eviction without decoded frame");
                sync.frameBufferEvicted(key, *record.deadline);
                if (!cvWaiting.recordOutcome(t, stats::Outcome::Skipped)) {
                    fail("statistics rejected eviction outcome");
                }
                ++observedDecisions;
            } else {
                fail("unsupported fixture event");
            }
        }
        if (observedDecode == 0 || observedDecisions == 0) fail("empty fixture");
        const auto syncView = sync.snapshot();
        const auto distribution = cvWaiting.snapshotBetween(0, lastTime, lastTime);
        const auto pacing = decoderPacing.snapshotBetween(0, lastTime, lastTime);
        if (!distribution.windowComplete || !pacing.intervals.windowComplete) {
            fail("truncated diagnostic fixture window");
        }
        if (syncView.decidedFramesTotal != observedDecisions ||
            distribution.sampleCount != syncView.presentedExactTotal ||
            distribution.outcomes[static_cast<std::size_t>(stats::Outcome::Missing)] !=
                syncView.dropMissingTotal ||
            distribution.outcomes[static_cast<std::size_t>(stats::Outcome::Skipped)] !=
                syncView.evictedTotal ||
            syncView.conflictingDecisionsTotal != 0) {
            fail("Statistics and Exact Sync lifecycle disagree");
        }
        const auto p95 = distribution.percentileUpper(95);
        // Human and machine-readable bounded output; no frame logs or JSON
        // copies, and no fabricated P95 when there were zero matched results.
        std::cout << "decoded=" << observedDecode
                  << " matched=" << syncView.presentedExactTotal
                  << " missing=" << syncView.dropMissingTotal
                  << " evicted=" << syncView.evictedTotal
                  << " valid_empty=" << syncView.validEmptyDetectionsTotal
                  << " late_after_drop=" << syncView.metadataAfterDroppedFrame
                  << " decision_count=" << syncView.decidedFramesTotal
                  << " distribution_success=" << distribution.sampleCount
                  << " outcome_missing=" <<
                     distribution.outcomes[static_cast<std::size_t>(stats::Outcome::Missing)]
                  << " outcome_evicted=" <<
                     distribution.outcomes[static_cast<std::size_t>(stats::Outcome::Skipped)]
                  << " frame_pacing_count=" << pacing.frameCount
                  << " pacing_rejected=" << rejectedPacing
                  << " p95_wait_ns=" << (p95 ? std::to_string(*p95) : "unavailable")
                  << " complete=" << (distribution.windowComplete ? "1" : "0")
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << "diagnostic_fixture_bridge FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
