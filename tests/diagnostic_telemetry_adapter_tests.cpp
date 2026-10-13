#include "diagnostics/adapter/TelemetryAdapter.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <string>

using namespace std::chrono_literals;
using reg::diagnostics::api::Availability;
using reg::diagnostics::api::Provenance;
using reg::diagnostics::adapter::Context;
using reg::diagnostics::adapter::makeSnapshot;
using reg::diagnostics::adapter::ResetIdentityState;
using reg::telemetry::Counters;
using reg::telemetry::Snapshot;

void require(bool x, const char* msg) { if (!x) throw std::runtime_error(msg); }
const auto& section(const reg::diagnostics::api::Snapshot& s, std::string_view name) {
    for (const auto& x : s.sections) if (x.name == name) return x;
    throw std::runtime_error("missing section");
}
const auto& metric(const reg::diagnostics::api::Section& s, std::string_view name) {
    for (const auto& x : s.metrics) if (x.name == name) return x;
    throw std::runtime_error("missing metric");
}
const auto& status(const reg::diagnostics::api::Section& s, std::string_view name) {
    for (const auto& x : s.statuses) if (x.name == name) return x;
    throw std::runtime_error("missing status");
}
Context context(std::int64_t seconds) {
    Context c;
    c.hostId = "reg-host"; c.processSessionId = "p-1";
    c.observedAtUtc = "2026-10-13T00:00:00Z"; c.sequence = 42;
    c.now = std::chrono::steady_clock::time_point{std::chrono::seconds{seconds}};
    c.dataOrigin = "synthetic"; c.testRunId = "test-1";
    return c;
}
void sample(Snapshot& s, std::int64_t seconds, const Counters& c) {
    s.samples.push_back({std::chrono::steady_clock::time_point{std::chrono::seconds{seconds}},c});
    s.counters = c;
}
void unknownAndZero() {
    Snapshot in;
    auto out = makeSnapshot(in, context(100));
    require(out.dataOrigin == "synthetic" && out.testRunId == "test-1" && out.sequence == 42,
            "DTO metadata");
    require(section(out, "rtsp").availability == Availability::Unavailable, "startup section unavailable");
    require(!metric(section(out, "video"), "decoder.decoded_frames_total").counter, "default zero is not observation");
    require(!status(section(out, "rtsp"), "rtspConnected").booleanValue, "default false unknown");
    Counters c; c.decoderSessions = 1;
    sample(in, 100, c);
    out = makeSnapshot(in, context(100));
    require(status(section(out, "rtsp"), "rtspConnected").booleanValue == false, "observed disconnected false");
    require(!status(section(out, "cv"), "cvSignalPresent").booleanValue, "no CV ever observed");
    require(metric(section(out, "video"), "decoder.decoded_frames_total").counter == 0, "observed zero exists");
    // An untimed Counters update may belong to a newer decoder generation;
    // the snapshot is timed only through the last periodic Sample.
    in.counters.decoderSessions = 2;
    in.counters.decodedFrames = 100000;
    out = makeSnapshot(in, context(100));
    require(metric(section(out, "video"), "decoder.decoded_frames_total").counter == 0 &&
            metric(section(out, "video"), "decoder.decoded_frames_total").resetId == "p-1.decoder:1.history:100000000000",
            "do not time-stamp a newer unsampled Counters generation");
    c.metadataPackets = 1; c.exactSync.validEmptyDetectionsTotal = 1;
    sample(in, 101, c);
    out = makeSnapshot(in, context(101));
    require(status(section(out, "cv"), "cvSignalPresent").booleanValue == false, "CV now observed and absent");
    require(metric(section(out, "cv"), "cv.valid_empty_detections_total").counter == 1,
            "accepted zero-target metadata represented");
}
void measuredRates() {
    Snapshot in;
    for (int t = 0; t <= 60; ++t) {
        Counters c; c.decoderSessions=1;c.decodedFrames=1000+60*t;
        c.rawPresentedFrames=50*t;c.overlayPresentedFrames=45*t;
        c.metadataPackets=20*t;c.recorderPacketsWritten=30*t;
        c.exactSync.sessionGeneration=5;c.exactSync.matchedPairs=18*t;
        c.rtspConnected = c.videoSignalPresent = true;
        sample(in,t,c);
    }
    const auto out = makeSnapshot(in,context(60));
    auto rate = [&](std::string_view s,std::string_view n,double v) {
        const auto& m=metric(section(out,s),n);
        require(m.value && std::abs(*m.value-v)<1e-9 && m.windowComplete==true,
                "wrong FPS/rate/window completeness");
        require(m.availability==Availability::Fresh && m.provenance==Provenance::Derived,
                "rate metadata");
    };
    for (auto suffix : {"1s","10s","60s"}) {
        rate("video",std::string("decoder.decoded_fps_")+suffix,60);
        rate("video",std::string("raw.presented_fps_")+suffix,50);
        rate("video",std::string("overlay.presented_decisions_rate_")+suffix,45);
        rate("cv",std::string("cv.packets_rate_")+suffix,20);
        rate("exact_sync",std::string("exact_sync.matched_pairs_rate_")+suffix,18);
        rate("recorder",std::string("recorder.packets_written_rate_")+suffix,30);
    }
    require(metric(section(out,"video"),"decoder.decoded_fps_60s").windowStartNs==0,
            "measured time start");
    require(metric(section(out,"video"),"decoder.decoded_fps_60s").windowEndNs==60'000'000'000LL,
            "measured time end");
}
void insufficientAndResets() {
    Snapshot s;
    Counters c;c.decoderSessions=1;c.decodedFrames=20;
    sample(s,10,c);
    auto out=makeSnapshot(s,context(10));
    require(!metric(section(out,"video"),"decoder.decoded_fps_1s").value,"one sample rate null");
    for(int t=11;t<=20;++t) {c.decodedFrames+=60;sample(s,t,c);}
    out=makeSnapshot(s,context(20));
    require(!metric(section(out,"video"),"decoder.decoded_fps_60s").value,"10s of data cannot prove 60s");
    require(metric(section(out,"video"),"decoder.decoded_fps_10s").value==60,
            "complete ten second rate");
    c.decoderSessions=2;c.decodedFrames=3;sample(s,21,c);
    out=makeSnapshot(s,context(21));
    require(!metric(section(out,"video"),"decoder.decoded_fps_1s").value,"session reset not bridged");
    require(metric(section(out,"video"),"decoder.decoded_frames_total").resetId=="p-1.decoder:2.history:10000000000",
            "decoder session reset id");
    c.decodedFrames=63;sample(s,22,c);
    out=makeSnapshot(s,context(22));
    require(metric(section(out,"video"),"decoder.decoded_fps_1s").value==60,
            "rate resumes post session");
    c.decodedFrames=0;sample(s,23,c);
    out=makeSnapshot(s,context(23));
    require(!metric(section(out,"video"),"decoder.decoded_fps_1s").value,
            "implicit decrease is not wraparound");
    c.decodedFrames=60;sample(s,24,c);
    out=makeSnapshot(s,context(24));
    require(metric(section(out,"video"),"decoder.decoded_fps_1s").value==60,
            "implicit reset recovery");
}
void staleAndSparse() {
    Snapshot s; Counters c;c.decoderSessions=1;c.decodedFrames=100;c.rtspConnected=true;
    sample(s,100,c);c.decodedFrames=160;sample(s,101,c);
    auto out=makeSnapshot(s,context(108));
    require(section(out,"video").availability==Availability::Stale,"section stale");
    require(!metric(section(out,"video"),"decoder.decoded_fps_1s").value,"stale rate absent");
    require(status(section(out,"rtsp"),"rtspConnected").availability==Availability::Stale,
            "rtsp stale status");
    Snapshot sparse;sample(sparse,0,c); c.decodedFrames=220;sample(sparse,2,c);
    out=makeSnapshot(sparse,context(2));
    require(!metric(section(out,"video"),"decoder.decoded_fps_1s").value,
            "large gap is not a complete 1s window");
}
void exactPopulationAndIdentity() {
    Snapshot s;Counters c;c.decoderSessions=1;
    auto& e=c.exactSync;
    e.sessionGeneration=7;e.decidedFramesTotal=10;e.presentedExactTotal=7;e.dropMissingTotal=2;
    e.evictedTotal=1;e.matchedPairs=7;e.latencySampleCount=4;
    e.arrivalDelayP50Ms=10;e.arrivalDelayP95Ms=22;e.arrivalDelayP99Ms=23;
    e.lastVideoKey=reg::media::FrameKey{UINT64_MAX,9007199254740993ULL};
    e.lastMetadataKey=reg::media::FrameKey{UINT64_MAX-1,9007199254740993ULL};
    e.lastMatchedKey=reg::media::FrameKey{UINT64_MAX,9007199254740993ULL};
    e.lastMatchedTargetCount=0;e.validEmptyDetectionsTotal=1;
    e.metadataObservationWindowComplete=false;e.metadataUnobservedAtCapacityTotal=2;
    sample(s,1,c);
    auto out=makeSnapshot(s,context(1));
    const auto& ex=section(out,"exact_sync");
    require(ex.lastFrameKey && ex.lastFrameKey->streamEpoch==UINT64_MAX &&
            ex.lastFrameKey->frameId==9007199254740993ULL,"full FrameKey uint64 fidelity");
    require(section(out,"cv").lastFrameKey->streamEpoch==UINT64_MAX-1,
            "CV FrameKey epoch cannot be assumed from video");
    const auto& coverage=metric(ex,"exact_sync.match_coverage");
    require(coverage.value && std::abs(*coverage.value-0.7)<1e-9 &&
            coverage.eligibleCount==10 && coverage.missingCount==2 && coverage.skippedCount==1,
            "decided-only denominator");
    require(!status(ex,"metadataObservationWindowComplete").booleanValue.value(),
            "pending censored followup not assumed complete");
    require(metric(ex,"exact_sync.pending_frame_count").kind ==
            reg::diagnostics::api::MetricKind::Gauge,
            "pending exact sync lifecycle count is a gauge, not a monotone counter");
    const auto& delay=metric(ex,"exact_sync.arrival_delay_matched_only_ms");
    require(delay.population=="matched_pairs_only_last_256" && delay.sampleCount==4 &&
            delay.p95==22 && delay.windowComplete==false,"matched-only exact sync delay");
    require(status(ex,"lastMatchedTargetCount").unsignedValue==0,
            "matched metadata with zero targets");
    c.exactSync.decidedFramesTotal=0;c.exactSync.presentedExactTotal=0;c.exactSync.dropMissingTotal=0;
    c.exactSync.evictedTotal=0;sample(s,2,c);
    out=makeSnapshot(s,context(2));
    require(!metric(section(out,"exact_sync"),"exact_sync.match_coverage").value,
            "zero denominator unavailable not zero");
}
void resetIdentityCodecAndRetention() {
    ResetIdentityState tracker;
    Snapshot s;
    auto ctx = context(10);
    ctx.resetIdentityState = &tracker;
    Counters c;
    c.decoderSessions = 4;
    c.exactSync.sessionGeneration = 7;
    c.decodedFrames = 1000;
    c.rawPresentedFrames = 900;
    c.exactSync.matchedPairs = 100;
    c.metadataPackets = 100;
    sample(s, 10, c);
    auto out = makeSnapshot(s, ctx);
    const auto decoderBefore = metric(section(out, "video"), "decoder.decoded_frames_total").resetId;
    const auto exactBefore = metric(section(out, "exact_sync"), "exact_sync.matched_pairs_total").resetId;
    const auto processBefore = metric(section(out, "cv"), "cv.packets_total").resetId;
    require(decoderBefore == "p-1.decoder:4" && exactBefore == "p-1.exact:7" &&
            processBefore == "p-1", "canonical path-safe reset IDs");
    auto isToken = [](std::string_view v) {
        if (v.empty() || v.size() > 128) return false;
        for (const unsigned char ch : v) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                ch == '.' || ch == ':') continue;
            return false;
        }
        return true;
    };
    auto validate = [&](const reg::diagnostics::api::Snapshot& v) {
        for (const auto& sec : v.sections)
            for (const auto& m : sec.metrics)
                require(isToken(m.resetId), "reset ID rejected by Agent B codec token policy");
    };
    validate(out);
    // No explicit session generation changes: all three owner domains have
    // independently observed unmarked resets.
    c.decodedFrames = 5; c.rawPresentedFrames = 6;
    c.exactSync.matchedPairs = 2;
    c.metadataPackets = 1;
    sample(s, 11, c); ctx.now = std::chrono::steady_clock::time_point{11s};
    out = makeSnapshot(s, ctx);
    const auto decoderAfter = metric(section(out, "video"), "decoder.decoded_frames_total").resetId;
    const auto exactAfter = metric(section(out, "exact_sync"), "exact_sync.matched_pairs_total").resetId;
    const auto processAfter = metric(section(out, "cv"), "cv.packets_total").resetId;
    require(decoderAfter == "p-1.decoder:4.inferred:1" &&
            exactAfter == "p-1.exact:7.inferred:1" &&
            processAfter == "p-1.inferred:1", "implicit reset requires a new emitted identity");
    require(metric(section(out, "video"), "decoder.decoded_fps_1s").resetId == decoderAfter &&
            metric(section(out, "exact_sync"), "exact_sync.matched_pairs_rate_1s").resetId == exactAfter &&
            metric(section(out, "cv"), "cv.packets_rate_1s").resetId == processAfter,
            "rates and cumulative counters agree on reset identity");
    require(!metric(section(out, "video"), "decoder.decoded_fps_1s").value,
            "counter decrease must not manufacture a rate");
    validate(out);
    // Once the pre-reset sample has been evicted, the owner state must STILL
    // emit the new identity. Retained history alone cannot prove the reset.
    s.samples.erase(s.samples.begin());
    c.decodedFrames = 65; c.exactSync.matchedPairs = 20; c.metadataPackets = 21;
    sample(s, 12, c); ctx.now = std::chrono::steady_clock::time_point{12s};
    out = makeSnapshot(s, ctx);
    require(metric(section(out, "video"), "decoder.decoded_frames_total").resetId == decoderAfter &&
            metric(section(out, "exact_sync"), "exact_sync.matched_pairs_total").resetId == exactAfter &&
            metric(section(out, "cv"), "cv.packets_total").resetId == processAfter,
            "identity must survive pre-reset history eviction");
    require(metric(section(out, "video"), "decoder.decoded_fps_1s").value == 60,
            "rate resumes on post-reset segment");
    // An untracked one-shot call intentionally gets a time-bound history ID,
    // not the long-lived owner-reset identity after ring rotation.
    auto fallback = makeSnapshot(s, context(12));
    require(metric(section(fallback, "video"), "decoder.decoded_frames_total").resetId != decoderBefore,
            "stateless fallback may not alias a previous session population");
    validate(fallback);
    // Explicit generations have disjoint canonical IDs and clear inferred seq.
    c.decoderSessions = 5; c.exactSync.sessionGeneration = 8;
    c.decodedFrames = 1; c.rawPresentedFrames = 1; c.exactSync.matchedPairs = 0;
    sample(s, 13, c); ctx.now = std::chrono::steady_clock::time_point{13s};
    out = makeSnapshot(s, ctx);
    require(metric(section(out, "video"), "decoder.decoded_frames_total").resetId == "p-1.decoder:5" &&
            metric(section(out, "exact_sync"), "exact_sync.matched_pairs_total").resetId == "p-1.exact:8",
            "explicit generation change must reset implicit sequence");
    validate(out);
    // Arbitrary process names can contain paths: reset labels must never
    // expose paths or be converted to JSON null by the codec.
    auto unsafeCtx = context(13);
    unsafeCtx.processSessionId = "C:/private/session/secret";
    out = makeSnapshot(s, unsafeCtx);
    validate(out);
    require(metric(section(out, "video"), "decoder.decoded_frames_total").resetId.find("private") == std::string::npos,
            "raw process path leaked through reset identity");
}
void siblingDomainResetDoesNotBridgeRate() {
    Snapshot s;
    ResetIdentityState tracker;
    auto ctx = context(10);
    ctx.resetIdentityState = &tracker;
    Counters c;
    c.decoderSessions = 1;
    c.metadataInvalid = 50;
    for (int t = 0; t <= 10; ++t) {
        c.decodedFrames = 100 + 60 * t;
        c.rawPresentedFrames = 50 + 50 * t;
        c.metadataPackets = 20 + 20 * t;
        sample(s, t, c);
    }
    auto out = makeSnapshot(s, ctx);
    require(metric(section(out, "video"), "decoder.decoded_fps_10s").value == 60,
            "pre-reset decoder FPS baseline");
    require(metric(section(out, "cv"), "cv.packets_rate_10s").value == 20,
            "pre-reset CV rate baseline");
    // Two different counters reset: decodedFrames and metadataPackets
    // themselves remain strictly increasing.
    c.decodedFrames += 60;
    c.rawPresentedFrames = 1;
    c.metadataPackets += 20;
    c.metadataInvalid = 0;
    sample(s, 11, c);
    ctx.now = std::chrono::steady_clock::time_point{11s};
    out = makeSnapshot(s, ctx);
    require(!metric(section(out, "video"), "decoder.decoded_fps_10s").value &&
            !metric(section(out, "cv"), "cv.packets_rate_10s").value,
            "a rate cannot cross another counter's reset in same owner domain");
    require(metric(section(out, "video"), "decoder.decoded_fps_10s").resetId ==
            metric(section(out, "video"), "decoder.decoded_frames_total").resetId,
            "rate and counter share post-reset decoder identity");
    c.decodedFrames += 60;
    c.rawPresentedFrames += 50;
    c.metadataPackets += 20;
    sample(s, 12, c);
    ctx.now = std::chrono::steady_clock::time_point{12s};
    out = makeSnapshot(s, ctx);
    require(metric(section(out, "video"), "decoder.decoded_fps_1s").value == 60 &&
            metric(section(out, "cv"), "cv.packets_rate_1s").value == 20,
            "single-second rates become measurable after reset boundary");
}
void uint64AndIsolation() {
    Snapshot s;Counters c;c.decoderSessions=1;c.reconnects=UINT64_MAX;
    c.recorderPacketsWritten=UINT64_MAX;c.metadataPackets=UINT64_MAX;
    sample(s,1,c);
    auto out=makeSnapshot(s,context(1));
    require(metric(section(out,"rtsp"),"rtsp.reconnects_total").counter==UINT64_MAX,
            "UINT64_MAX must not pass through double");
    require(metric(section(out,"recorder"),"recorder.packets_written_total").counter==UINT64_MAX,
            "recorder UINT64_MAX");
    require(section(out,"ui").availability==Availability::Unsupported,"uninstrumented UI explicit");
    require(metric(section(out,"video"),"raw.presented_frames_total").population.find("not_scanout")!=std::string::npos,
            "physical scanout not inferred");
    require(metric(section(out,"cv"),"cv.packets_total").owner=="reg", "no Jetson invention");
}
int main(){
 try {unknownAndZero();measuredRates();insufficientAndResets();staleAndSparse();
      exactPopulationAndIdentity();uint64AndIsolation();resetIdentityCodecAndRetention();siblingDomainResetDoesNotBridgeRate();
      std::cout<<"diagnostic_telemetry_adapter_tests: PASS\n";return 0;}
 catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
