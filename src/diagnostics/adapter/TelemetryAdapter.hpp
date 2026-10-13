#pragma once

// Offline, single-threaded conversion of a previously copied TelemetryModel
// snapshot. This class must be called by the coordinator's background sampler,
// never directly from a decode, presentation or CV reception callback.

#include "diagnostics/api/DiagnosticContract.hpp"
#include "diagnostics/stats/Statistics.hpp"
#include "telemetry/TelemetryModel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reg::diagnostics::adapter {

class ResetIdentityState;

struct Context {
    std::string hostId;
    std::string processSessionId;
    std::string observedAtUtc;
    std::uint64_t sequence{};
    std::string dataOrigin{"live"}; // use "synthetic" only for explicit tests
    std::optional<std::string> testRunId;
    std::optional<std::uint64_t> monotonicUptimeMs;
    std::chrono::steady_clock::time_point now{};
    std::chrono::milliseconds freshness{2000};
    // Optional persistent owner-local identity tracker, owned by the
    // coordinator's single low-priority sampler thread, never shared with
    // decoder/render callbacks. The stateless fallback uses intentionally
    // short-lived IDs so it cannot alias reset populations after eviction.
    ResetIdentityState* resetIdentityState{};
};

// Reset identity belongs to the measurement owner, not to the JSON codec.
// The retained TelemetryModel sample ring is insufficient to reconstruct
// a stable implicit-reset sequence after old observations have been pruned.
// Keep this state with the *one* background sampler for the whole process.
// The current counters are not time-stamped: only consume timed samples.
class ResetIdentityState final {
public:
    void observe(const telemetry::Snapshot& input, const Context& ctx) {
        if (processSession_ != ctx.processSessionId) {
            *this = ResetIdentityState{};
            processSession_ = ctx.processSessionId;
        }
        for (const auto& sample : input.samples) {
            const auto at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                sample.time.time_since_epoch()).count();
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                ctx.now.time_since_epoch()).count();
            if (at < 0 || at > now) continue;
            if (lastTime_ && at <= *lastTime_) continue; // already ingested
            const auto& cur = sample.counters;
            if (last_) {
                const auto& prev = *last_;
                if (cur.decoderSessions != prev.decoderSessions) {
                    decoderResets_ = 0;
                    decoderResetAtNs_ = at;
                } else if (decoderDecreased(cur, prev)) {
                    ++decoderResets_;
                    decoderResetAtNs_ = at;
                }
                if (cur.exactSync.sessionGeneration != prev.exactSync.sessionGeneration) {
                    exactResets_ = 0;
                    exactResetAtNs_ = at;
                } else if (exactDecreased(cur.exactSync, prev.exactSync)) {
                    ++exactResets_;
                    exactResetAtNs_ = at;
                }
                if (processDecreased(cur, prev)) {
                    ++processResets_;
                    processResetAtNs_ = at;
                }
            }
            last_ = cur;
            lastTime_ = at;
        }
    }

    [[nodiscard]] std::uint64_t processResets() const noexcept { return processResets_; }
    [[nodiscard]] std::uint64_t decoderResets() const noexcept { return decoderResets_; }
    [[nodiscard]] std::uint64_t exactResets() const noexcept { return exactResets_; }
    [[nodiscard]] std::optional<std::int64_t> processResetAtNs() const noexcept {
        return processResetAtNs_;
    }
    [[nodiscard]] std::optional<std::int64_t> decoderResetAtNs() const noexcept {
        return decoderResetAtNs_;
    }
    [[nodiscard]] std::optional<std::int64_t> exactResetAtNs() const noexcept {
        return exactResetAtNs_;
    }

private:
    // Detect the observed decreases at the owner-domain level. A missed
    // decrease/recovery between periodic samples cannot be inferred and needs
    // future producer instrumentation; a process restart requires a new ID.
    static bool processDecreased(const telemetry::Counters& a, const telemetry::Counters& b) {
        return a.decoderSessions < b.decoderSessions || a.reconnects < b.reconnects ||
            a.metadataPackets < b.metadataPackets || a.metadataInvalid < b.metadataInvalid ||
            a.metadataDuplicates < b.metadataDuplicates ||
            a.metadataSequenceGaps < b.metadataSequenceGaps ||
            a.metadataOutOfOrder < b.metadataOutOfOrder ||
            a.metadataStoreEvictions < b.metadataStoreEvictions ||
            a.recorderPacketsWritten < b.recorderPacketsWritten ||
            a.recorderQueueDrops < b.recorderQueueDrops ||
            a.recorderMetadataWritten < b.recorderMetadataWritten ||
            a.recorderMetadataQueueDrops < b.recorderMetadataQueueDrops ||
            a.recorderFailures < b.recorderFailures;
    }
    static bool decoderDecreased(const telemetry::Counters& a, const telemetry::Counters& b) {
        return a.decodedFrames < b.decodedFrames ||
            a.rawPresentedFrames < b.rawPresentedFrames ||
            a.overlayPresentedFrames < b.overlayPresentedFrames ||
            a.overlayMissingMetadataDrops < b.overlayMissingMetadataDrops ||
            a.overlayBufferEvictions < b.overlayBufferEvictions ||
            a.overlayMissingIdentity < b.overlayMissingIdentity;
    }
    static bool exactDecreased(const diagnostics::ExactSyncSnapshot& a,
                               const diagnostics::ExactSyncSnapshot& b) {
        return a.matchedPairs < b.matchedPairs ||
            a.eligibleFramesTotal < b.eligibleFramesTotal ||
            a.presentedExactTotal < b.presentedExactTotal ||
            a.dropMissingTotal < b.dropMissingTotal ||
            a.evictedTotal < b.evictedTotal ||
            a.metadataObservedTotal < b.metadataObservedTotal ||
            a.validEmptyDetectionsTotal < b.validEmptyDetectionsTotal ||
            a.metadataUnobservedAtExpiryTotal < b.metadataUnobservedAtExpiryTotal ||
            a.metadataUnobservedAtCapacityTotal < b.metadataUnobservedAtCapacityTotal ||
            a.metadataUnobservedAtClearTotal < b.metadataUnobservedAtClearTotal ||
            a.unknownAtExpiryTotal < b.unknownAtExpiryTotal ||
            a.unknownAtClearTotal < b.unknownAtClearTotal;
    }
    std::string processSession_;
    std::optional<std::int64_t> lastTime_;
    std::optional<telemetry::Counters> last_;
    std::uint64_t processResets_{};
    std::uint64_t decoderResets_{};
    std::uint64_t exactResets_{};
    std::optional<std::int64_t> processResetAtNs_;
    std::optional<std::int64_t> decoderResetAtNs_;
    std::optional<std::int64_t> exactResetAtNs_;
};

namespace detail {
inline std::int64_t ns(std::chrono::steady_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        point.time_since_epoch()).count();
}
inline api::Availability ageAvailability(std::int64_t now, std::optional<std::int64_t> last,
                                         std::int64_t freshness) {
    if (!last || *last > now || freshness <= 0) return api::Availability::Unavailable;
    return static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(*last) >
        static_cast<std::uint64_t>(freshness) ? api::Availability::Stale : api::Availability::Fresh;
}
inline std::optional<std::uint64_t> ageMs(std::int64_t now, std::optional<std::int64_t> last) {
    if (!last || *last > now) return std::nullopt;
    return (static_cast<std::uint64_t>(now) -
            static_cast<std::uint64_t>(*last)) / 1'000'000ULL;
}
inline api::FrameKey key(media::FrameKey k) { return {k.streamEpoch, k.frameId}; }
// Match Agent B's ASCII label whitelist. Raw paths, URLs, whitespace or
// user-controlled process identifiers must never leak into reset IDs.
inline bool asciiToken(std::string_view id) {
    if (id.empty() || id.size() > 30) return false;
    for (const unsigned char ch : id) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
            ch == '.' || ch == ':') continue;
        return false;
    }
    return true;
}
inline std::string safeProcessId(std::string_view id) {
    if (asciiToken(id)) return std::string(id);
    // Bounded deterministic opaque surrogate; never embed an arbitrary path.
    std::uint64_t digest = 14695981039346656037ULL;
    for (const unsigned char ch : id) {
        digest ^= ch;
        digest *= 1099511628211ULL;
    }
    return "opaque:" + std::to_string(digest);
}
inline std::string withResets(std::string id, std::uint64_t resets) {
    if (resets) id += ".inferred:" + std::to_string(resets);
    return id;
}
inline bool finite(double x) { return std::isfinite(x); }
inline api::Metric makeMetric(std::string name, std::string unit, std::string population,
                              std::string resetScope, std::string resetId,
                              api::MetricKind kind, api::Provenance provenance,
                              api::Availability availability) {
    api::Metric m{};
    m.name = std::move(name); m.unit = std::move(unit); m.owner = "reg";
    m.population = std::move(population); m.clockDomain = "reg-steady";
    m.resetScope = std::move(resetScope); m.resetId = std::move(resetId);
    m.kind = kind; m.provenance = provenance; m.availability = availability;
    return m;
}
inline void counter(api::Section& sec, std::string name, std::uint64_t count,
                    std::string population, std::string scope, std::string id,
                    api::Availability state) {
    auto m = makeMetric(std::move(name), "count", std::move(population),
                        std::move(scope), std::move(id), api::MetricKind::Counter,
                        api::Provenance::Measured, state);
    if (state != api::Availability::Unavailable) m.counter = count;
    sec.metrics.push_back(std::move(m));
}
inline void gauge(api::Section& sec, std::string name, std::optional<double> value,
                  std::string unit, std::string population, std::string scope,
                  std::string id, api::Availability state,
                  api::Provenance provenance = api::Provenance::Measured) {
    auto m = makeMetric(std::move(name), std::move(unit), std::move(population),
                        std::move(scope), std::move(id), api::MetricKind::Gauge, provenance, state);
    if (state != api::Availability::Unavailable && value && finite(*value)) m.value = *value;
    else if (!value || !finite(value.value_or(0))) m.availability = api::Availability::Unavailable;
    sec.metrics.push_back(std::move(m));
}
inline void status(api::Section& sec, std::string name, std::optional<bool> value,
                   api::Availability state) {
    api::Status s{}; s.name = std::move(name); s.availability = state;
    if (state != api::Availability::Unavailable) s.booleanValue = value;
    sec.statuses.push_back(std::move(s));
}
inline void numberStatus(api::Section& sec, std::string name, std::optional<std::uint64_t> value,
                         api::Availability state) {
    api::Status s{}; s.name = std::move(name); s.availability = state;
    if (state != api::Availability::Unavailable) s.unsignedValue = value;
    sec.statuses.push_back(std::move(s));
}

// Rebuild Statistics Core windows from the authoritative 250-ms TelemetryModel
// history. Never interpret one periodic counter sample as a frame timestamp.
template <typename Value, typename Generation>
void rates(api::Section& sec, const telemetry::Snapshot& src, const Context& ctx,
           std::string name, std::string population, std::string scope, std::string resetId,
           Value value, Generation generation, bool generationStable = true,
           std::optional<std::int64_t> lastDomainResetNs = std::nullopt) {
    constexpr std::array<std::pair<std::string_view, std::int64_t>, 3> windows{{
        {"1s", stats::kOneSecondNs}, {"10s", stats::kTenSecondsNs},
        {"60s", stats::kSixtySecondsNs}}};
    const auto now = ns(ctx.now);
    const auto freshness = std::chrono::duration_cast<std::chrono::nanoseconds>(ctx.freshness).count();
    const auto generationNow = src.samples.empty() ? 0 : generation(src.samples.back().counters);
    stats::MetricIdentity identity{std::string(name), "count/s", "reg", population,
                                   "reg-steady", ctx.processSessionId, generationNow,
                                   stats::MetricKind::Counter};
    stats::CounterRateWindow window(std::move(identity),
                                   std::max<std::size_t>(2, src.samples.size() + 1),
                                   std::max<std::int64_t>(1, freshness));
    bool orderValid = true;
    std::optional<std::int64_t> previous;
    for (const auto& sample : src.samples) {
        const auto t = ns(sample.time);
        if (t < 0 || t > now || (previous && t <= *previous)) { orderValid = false; break; }
        previous = t;
        if (!window.observe(t, value(sample.counters), generation(sample.counters))) {
            orderValid = false; break;
        }
    }
    for (const auto& [suffix, width] : windows) {
        auto m = makeMetric(name + "_" + std::string(suffix), "frames/s", population,
                            scope, resetId, api::MetricKind::Rate,
                            api::Provenance::Derived, api::Availability::Unavailable);
        if (name.find("fps") == std::string::npos) m.unit = "count/s";
        const auto start = now >= width ? now - width : std::int64_t{0};
        m.windowStartNs = start; m.windowEndNs = now; m.windowComplete = false;
        // A reset of a sibling counter in this owner domain changes the
        // shared reset ID too. Never allow a rate to bridge that boundary
        // just because this particular numerator remained monotonic.
        const bool crossesReset = lastDomainResetNs && *lastDomainResetNs > start &&
            *lastDomainResetNs <= now;
        if (now >= width && orderValid && generationStable && !crossesReset) {
            const auto r = window.snapshotBetween(start, now, now);
            std::optional<std::int64_t> first, last;
            std::optional<std::int64_t> prev;
            bool regular = true;
            for (const auto& s : src.samples) {
                const auto t = ns(s.time);
                if (t < start || t > now || generation(s.counters) != generationNow) continue;
                if (!first) first = t;
                if (prev && t - *prev > 1'500'000'000LL) regular = false;
                last = t; prev = t;
            }
            if (first && last) {
                m.windowStartNs = *first; m.windowEndNs = *last;
                m.sampleCount = r.observations;
            }
            // Only a covered, fresh, same-generation window is a valid rate.
            constexpr std::int64_t kBoundaryTolerance = 500'000'000LL;
            const auto tolerance = std::min<std::int64_t>(kBoundaryTolerance, width / 4);
            const bool covered = first && last && *first <= start + tolerance &&
                *last >= now - tolerance && *last - *first >= width - 2 * tolerance;
            const bool complete = covered && regular && r.windowComplete &&
                r.sourceAvailability == stats::Availability::Fresh && r.ratePerSecond &&
                std::isfinite(*r.ratePerSecond);
            m.windowComplete = complete;
            if (complete) { m.value = *r.ratePerSecond; m.availability = api::Availability::Fresh; }
            else if (r.sourceAvailability == stats::Availability::Stale)
                m.availability = api::Availability::Stale;
        }
        sec.metrics.push_back(std::move(m));
    }
}
} // namespace detail

[[nodiscard]] inline api::Snapshot makeSnapshot(const telemetry::Snapshot& input,
                                                  const Context& ctx) {
    using namespace detail;
    api::Snapshot out{};
    out.hostId = ctx.hostId; out.processSessionId = ctx.processSessionId;
    out.observedAtUtc = ctx.observedAtUtc; out.sequence = ctx.sequence;
    out.dataOrigin = ctx.dataOrigin; out.testRunId = ctx.testRunId;
    out.monotonicUptimeMs = ctx.monotonicUptimeMs;
    const auto now = ns(ctx.now);
    const auto freshness = std::chrono::duration_cast<std::chrono::nanoseconds>(ctx.freshness).count();
    std::optional<std::int64_t> last;
    if (!input.samples.empty()) last = ns(input.samples.back().time);
    const auto state = ageAvailability(now, last, freshness);
    // No timestamp accompanies Counters; avoid presenting the untimed latest
    // Counters as fresh if the last observable history sample is stale.
    // Counters has no measurement timestamp. Only the periodic Sample carries
    // a trustworthy time: never mix an un-timed newer Counters generation with
    // a previous Sample when reporting snapshot freshness/reset identity.
    const auto& c = input.samples.empty() ? input.counters : input.samples.back().counters;
    // For a long-running live sampler, pass persistent ResetIdentityState in
    // Context. Without it, incorporate the oldest retained observation into
    // every reset identity, making the ID explicitly short-lived rather than
    // silently aliasing distinct populations after a historical reset falls
    // outside TelemetryModel retention.
    ResetIdentityState temporaryState;
    auto& resets = ctx.resetIdentityState ? *ctx.resetIdentityState : temporaryState;
    resets.observe(input, ctx);
    const auto prefix = safeProcessId(ctx.processSessionId);
    const auto ephemeral = ctx.resetIdentityState || input.samples.empty()
        ? std::string{} : ".history:" + std::to_string(ns(input.samples.front().time));
    const auto process = withResets(prefix + ephemeral, resets.processResets());
    const auto decoder = withResets(prefix + ".decoder:" +
        std::to_string(c.decoderSessions) + ephemeral, resets.decoderResets());
    const auto exact = withResets(prefix + ".exact:" +
        std::to_string(c.exactSync.sessionGeneration) + ephemeral, resets.exactResets());
    const auto age = ageMs(now, last);
    auto section = [&](std::string name) {
        api::Section s{}; s.name = std::move(name); s.availability = state;
        s.dataAgeMs = age; return s;
    };
    const bool decoderStable = !input.samples.empty() &&
        c.decoderSessions == input.samples.back().counters.decoderSessions;
    const bool exactStable = !input.samples.empty() &&
        c.exactSync.sessionGeneration == input.samples.back().counters.exactSync.sessionGeneration;

    auto rtsp = section("rtsp");
    const bool rtspObserved = c.rtspConnected || c.decoderSessions > 0 || c.reconnects > 0 ||
        std::any_of(input.samples.begin(), input.samples.end(),
                    [](const auto& s) { return s.counters.rtspConnected; });
    status(rtsp, "rtspConnected", rtspObserved ? std::optional{c.rtspConnected} : std::nullopt,
           rtspObserved ? state : api::Availability::Unavailable);
    counter(rtsp, "rtsp.decoder_sessions_total", c.decoderSessions, "decoder_sessions",
            "process", process, state);
    counter(rtsp, "rtsp.reconnects_total", c.reconnects, "reconnections", "process", process, state);
    out.sections.push_back(std::move(rtsp));

    auto video = section("video");
    const bool videoObserved = c.videoSignalPresent || c.decodedFrames > 0 ||
        c.videoFrameAgeMs > 0 || std::any_of(input.samples.begin(), input.samples.end(),
                       [](const auto& s) { return s.counters.videoSignalPresent; });
    status(video, "videoSignalPresent", videoObserved ? std::optional{c.videoSignalPresent} : std::nullopt,
           videoObserved ? state : api::Availability::Unavailable);
    gauge(video, "video.last_frame_age_ms", videoObserved ? std::optional<double>(c.videoFrameAgeMs) :
          std::nullopt, "ms", "last_decoded_video_frame", "decoder_session", decoder, state);
    const auto videoCounter = [&](std::string name, std::uint64_t value, std::string population) {
        counter(video, std::move(name), value, std::move(population), "decoder_session", decoder, state);
    };
    videoCounter("decoder.decoded_frames_total", c.decodedFrames, "decoded_frames");
    videoCounter("raw.presented_frames_total", c.rawPresentedFrames, "raw_present_decisions_not_scanout");
    videoCounter("overlay.presented_decisions_total", c.overlayPresentedFrames,
                 "overlay_present_decisions_not_scanout");
    videoCounter("overlay.missing_metadata_drops_total", c.overlayMissingMetadataDrops, "overlay_due_frames");
    videoCounter("overlay.buffer_evictions_total", c.overlayBufferEvictions, "overlay_buffer_entries");
    videoCounter("overlay.missing_identity_total", c.overlayMissingIdentity, "overlay_unidentified_frames");
    gauge(video, "overlay.buffer_depth", static_cast<double>(c.overlayBufferDepth), "frames",
          "overlay_buffer", "decoder_session", decoder, state);
    gauge(video, "overlay.configured_delay_ms", static_cast<double>(c.configuredOverlayDelayMs),
          "ms", "overlay_configuration", "process", process, state);
    rates(video, input, ctx, "decoder.decoded_fps", "decoded_frames", "decoder_session", decoder,
          [](const auto& x) { return x.decodedFrames; },
          [](const auto& x) { return x.decoderSessions; }, decoderStable, resets.decoderResetAtNs());
    rates(video, input, ctx, "raw.presented_fps", "raw_present_decisions_not_scanout",
          "decoder_session", decoder,
          [](const auto& x) { return x.rawPresentedFrames; },
          [](const auto& x) { return x.decoderSessions; }, decoderStable, resets.decoderResetAtNs());
    rates(video, input, ctx, "overlay.presented_decisions_rate", "overlay_present_decisions_not_scanout",
          "decoder_session", decoder,
          [](const auto& x) { return x.overlayPresentedFrames; },
          [](const auto& x) { return x.decoderSessions; }, decoderStable, resets.decoderResetAtNs());
    out.sections.push_back(std::move(video));

    auto cv = section("cv");
    const bool cvObserved = c.cvSignalPresent || c.metadataPackets > 0 || c.metadataInvalid > 0 ||
        std::any_of(input.samples.begin(), input.samples.end(),
                    [](const auto& s) { return s.counters.cvSignalPresent; });
    status(cv, "cvSignalPresent", cvObserved ? std::optional{c.cvSignalPresent} : std::nullopt,
           cvObserved ? state : api::Availability::Unavailable);
    gauge(cv, "cv.last_packet_age_ms", cvObserved ? std::optional<double>(c.cvPacketAgeMs) :
          std::nullopt, "ms", "last_received_cv_packet", "process", process, state);
    const auto cvCounter = [&](std::string name, std::uint64_t value, std::string population) {
        counter(cv, std::move(name), value, std::move(population), "process", process, state);
    };
    cvCounter("cv.packets_total", c.metadataPackets, "received_cv_packets");
    cvCounter("cv.invalid_total", c.metadataInvalid, "rejected_cv_packets");
    cvCounter("cv.duplicates_total", c.metadataDuplicates, "duplicate_cv_packets");
    cvCounter("cv.sequence_gaps_total", c.metadataSequenceGaps, "sequence_gap_observations_not_packet_loss");
    cvCounter("cv.out_of_order_total", c.metadataOutOfOrder, "out_of_order_cv_packets");
    cvCounter("cv.store_evictions_total", c.metadataStoreEvictions, "metadata_store_entries");
    counter(cv, "cv.valid_empty_detections_total", c.exactSync.validEmptyDetectionsTotal,
            "accepted_cv_metadata_with_zero_targets", "exact_sync_session", exact, state);
    gauge(cv, "cv.metadata_store_depth", static_cast<double>(c.metadataStoreDepth), "packets",
          "metadata_store", "process", process, state);
    if (c.exactSync.lastMetadataKey && state != api::Availability::Unavailable)
        cv.lastFrameKey = key(*c.exactSync.lastMetadataKey);
    rates(cv, input, ctx, "cv.packets_rate", "received_cv_packets", "process", process,
          [](const auto& x) { return x.metadataPackets; },
          [](const auto&) -> std::uint64_t { return 0; }, true, resets.processResetAtNs());
    out.sections.push_back(std::move(cv));

    auto sync = section("exact_sync");
    if (c.exactSync.lastVideoKey && state != api::Availability::Unavailable)
        sync.lastFrameKey = key(*c.exactSync.lastVideoKey);
    const auto syncCounter = [&](std::string name, std::uint64_t value, std::string population) {
        counter(sync, std::move(name), value, std::move(population), "exact_sync_session", exact, state);
    };
    const auto& e = c.exactSync;
    syncCounter("exact_sync.matched_pairs_total", e.matchedPairs, "exact_key_matched_pairs");
    syncCounter("exact_sync.presented_exact_total", e.presentedExactTotal, "exact_overlay_decisions_not_scanout");
    syncCounter("exact_sync.eligible_frames_total", e.eligibleFramesTotal, "overlay_admitted_frames");
    syncCounter("exact_sync.decided_frames_total", e.decidedFramesTotal, "overlay_decisions");
    syncCounter("exact_sync.drop_missing_total", e.dropMissingTotal, "overlay_decisions_missing_cv");
    syncCounter("exact_sync.evicted_total", e.evictedTotal, "overlay_decisions_evicted");
    syncCounter("exact_sync.missing_identity_total", e.missingIdentityTotal, "overlay_frames_no_framekey");
    syncCounter("exact_sync.rejected_key_mismatches_total", e.rejectedKeyMismatches, "unequal_framekey_pairs");
    syncCounter("exact_sync.metadata_observed_total", e.metadataObservedTotal, "accepted_distinct_framekeys_retained");
    syncCounter("exact_sync.metadata_late_observed_total", e.metadataLateObservedTotal, "late_metadata_observations");
    syncCounter("exact_sync.metadata_after_dropped_frame_total", e.metadataAfterDroppedFrame,
                "metadata_for_already_dropped_frames");
    syncCounter("exact_sync.metadata_unobserved_at_expiry_total", e.metadataUnobservedAtExpiryTotal,
                "expiry_without_accepted_cv_not_proven_loss");
    syncCounter("exact_sync.metadata_unobserved_at_capacity_total", e.metadataUnobservedAtCapacityTotal,
                "capacity_censored_followup");
    syncCounter("exact_sync.metadata_unobserved_at_clear_total", e.metadataUnobservedAtClearTotal,
                "flush_censored_followup");
    syncCounter("exact_sync.capacity_censored_total", e.capacityCensoredTotal, "capacity_censored_pending");
    syncCounter("exact_sync.unknown_at_expiry_total", e.unknownAtExpiryTotal, "unknown_pending_on_expiry");
    syncCounter("exact_sync.unknown_at_clear_total", e.unknownAtClearTotal, "unknown_pending_on_clear");
    // These are instantaneous bounded queue/dependency depths, not monotone
    // cumulative counters. Never publish them as MetricKind::Counter.
    gauge(sync, "exact_sync.pending_frame_count", static_cast<double>(e.pendingFrameCount),
          "frames", "pending_frames", "exact_sync_session", exact, state);
    gauge(sync, "exact_sync.decided_awaiting_metadata_count",
          static_cast<double>(e.decidedAwaitingMetadataCount), "frames",
          "retained_decisions_without_cv", "exact_sync_session", exact, state);
    const bool syncObserved = e.sessionGeneration != 0 || e.eligibleFramesTotal != 0 ||
        e.decidedFramesTotal != 0 || e.metadataObservedTotal != 0 || e.matchedPairs != 0;
    status(sync, "metadataObservationWindowComplete",
           syncObserved ? std::optional<bool>(e.metadataObservationWindowComplete) : std::nullopt,
           syncObserved ? state : api::Availability::Unavailable);
    const bool hasDenom = e.decidedFramesTotal != 0;
    gauge(sync, "exact_sync.match_coverage", hasDenom && e.presentedExactTotal <= e.decidedFramesTotal
          ? std::optional<double>(static_cast<double>(e.presentedExactTotal) /
                                  static_cast<double>(e.decidedFramesTotal)) : std::nullopt,
          "ratio", "decided_overlay_frames_only", "exact_sync_session", exact, state,
          api::Provenance::Derived);
    if (hasDenom) {
        auto& coverage = sync.metrics.back();
        coverage.eligibleCount = e.decidedFramesTotal;
        coverage.missingCount = e.dropMissingTotal;
        coverage.skippedCount = e.evictedTotal;
        coverage.sampleCount = e.presentedExactTotal;
    }
    // The legacy ring has only <=256 successful matched samples. It is a
    // bounded *last-N* set, not a 1/10/60-second or all-CV latency histogram.
    auto wait = makeMetric("exact_sync.arrival_delay_matched_only_ms", "ms", "matched_pairs_only_last_256",
                           "exact_sync_session", exact, api::MetricKind::Distribution,
                           api::Provenance::Measured, state);
    wait.windowComplete = false; // right-censored selection, not all arrivals
    if (state != api::Availability::Unavailable && e.latencySampleCount > 0 &&
        finite(e.arrivalDelayP50Ms) && finite(e.arrivalDelayP95Ms) && finite(e.arrivalDelayP99Ms)) {
        wait.sampleCount = e.latencySampleCount;
        wait.p50 = e.arrivalDelayP50Ms; wait.p95 = e.arrivalDelayP95Ms;
        wait.p99 = e.arrivalDelayP99Ms;
    } else wait.availability = api::Availability::Unavailable;
    sync.metrics.push_back(std::move(wait));
    gauge(sync, "exact_sync.last_arrival_relative_to_decode_ms",
          e.hasTimingSample ? std::optional<double>(e.lastArrivalRelativeToDecodeMs) : std::nullopt,
          "ms", "latest_matched_pair_only_signed", "exact_sync_session", exact, state);
    gauge(sync, "exact_sync.last_playout_wait_ms",
          e.hasTimingSample ? std::optional<double>(e.lastPlayoutWaitMs) : std::nullopt,
          "ms", "latest_matched_pair_only", "exact_sync_session", exact, state);
    numberStatus(sync, "lastMatchedTargetCount",
                 e.lastMatchedKey ? std::optional<std::uint64_t>(e.lastMatchedTargetCount) : std::nullopt,
                 e.lastMatchedKey ? state : api::Availability::Unavailable);
    rates(sync, input, ctx, "exact_sync.matched_pairs_rate", "exact_key_matched_pairs",
          "exact_sync_session", exact,
          [](const auto& x) { return x.exactSync.matchedPairs; },
          [](const auto& x) { return x.exactSync.sessionGeneration; }, exactStable, resets.exactResetAtNs());
    out.sections.push_back(std::move(sync));

    auto recorder = section("recorder");
    const auto rec = [&](std::string name, std::uint64_t val, std::string population) {
        counter(recorder, std::move(name), val, std::move(population), "process", process, state);
    };
    rec("recorder.packets_written_total", c.recorderPacketsWritten, "written_compressed_packets");
    rec("recorder.queue_drops_total", c.recorderQueueDrops, "dropped_recording_packets");
    rec("recorder.metadata_written_total", c.recorderMetadataWritten, "written_metadata_entries");
    rec("recorder.metadata_queue_drops_total", c.recorderMetadataQueueDrops, "dropped_recording_metadata");
    rec("recorder.failures_total", c.recorderFailures, "recorder_failures");
    gauge(recorder, "recorder.queue_depth", static_cast<double>(c.recorderQueueDepth),
          "packets", "recorder_queue", "process", process, state);
    rates(recorder, input, ctx, "recorder.packets_written_rate", "written_compressed_packets",
          "process", process,
          [](const auto& x) { return x.recorderPacketsWritten; },
          [](const auto&) -> std::uint64_t { return 0; }, true, resets.processResetAtNs());
    out.sections.push_back(std::move(recorder));

    auto ui = section("ui"); // no NetImgui/client liveness hooks in TelemetryModel
    ui.availability = api::Availability::Unsupported;
    ui.dataAgeMs = std::nullopt;
    out.sections.push_back(std::move(ui));
    return out;
}

} // namespace reg::diagnostics::adapter
