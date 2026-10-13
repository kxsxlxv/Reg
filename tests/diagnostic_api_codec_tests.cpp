#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace reg::diagnostics::api;
using reg::diagnostics::api::codec::respond;

void contains(const std::string& text, std::string_view fragment) {
    if (text.find(fragment) == std::string::npos) {
        std::cerr << "missing JSON fragment: " << fragment << "\n";
        std::abort();
    }
}
void excludes(const std::string& text, std::string_view fragment) {
    if (text.find(fragment) != std::string::npos) {
        std::cerr << "unexpected JSON fragment: " << fragment << "\n";
        std::abort();
    }
}

// Dependency-free recursive JSON grammar validation (RFC 8259). It validates
// all response bodies, including OpenAPI and every error, without GPU or HTTP.
class JsonSyntax {
public:
    explicit JsonSyntax(std::string_view src) : src_(src) {}
    bool valid() { skip(); return value(0) && (skip(), pos_ == src_.size()); }
private:
    std::string_view src_;
    std::size_t pos_ = 0;
    void skip() { while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\n' || src_[pos_] == '\t' || src_[pos_] == '\r')) ++pos_; }
    bool take(char c) { if (pos_ < src_.size() && src_[pos_] == c) { ++pos_; return true; } return false; }
    bool match(std::string_view word) { if (src_.substr(pos_).starts_with(word)) { pos_ += word.size(); return true; } return false; }
    bool string() {
        if (!take('"')) return false;
        while (pos_ < src_.size()) {
            const auto c = static_cast<unsigned char>(src_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20U) return false;
            if (c != '\\') continue;
            if (pos_ == src_.size()) return false;
            const char e = src_[pos_++];
            if (std::string_view{"\"\\/bfnrt"}.find(e) != std::string_view::npos) continue;
            if (e != 'u' || pos_ + 4 > src_.size()) return false;
            for (int i = 0; i < 4; ++i) {
                const char h = src_[pos_++];
                if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
            }
        }
        return false;
    }
    bool number() {
        take('-');
        if (take('0')) {
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') return false;
        } else {
            if (pos_ == src_.size() || src_[pos_] < '1' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (take('.')) {
            if (pos_ == src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (pos_ == src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        return true;
    }
    bool value(int depth) {
        if (depth > 70) return false;
        skip();
        if (pos_ == src_.size()) return false;
        if (src_[pos_] == '"') return string();
        if (take('{')) {
            skip(); if (take('}')) return true;
            do { skip(); if (!string()) return false; skip(); if (!take(':') || !value(depth + 1)) return false; skip(); if (take('}')) return true; }
            while (take(','));
            return false;
        }
        if (take('[')) {
            skip(); if (take(']')) return true;
            do { if (!value(depth + 1)) return false; skip(); if (take(']')) return true; }
            while (take(','));
            return false;
        }
        if (match("true") || match("false") || match("null")) return true;
        return src_[pos_] == '-' || (src_[pos_] >= '0' && src_[pos_] <= '9') ? number() : false;
    }
};
void ok(const HttpResponse& response, int status = 200) {
    assert(response.statusCode == status);
    assert(response.contentType == "application/json; charset=utf-8");
    assert(response.body.size() <= reg::diagnostics::api::codec::kMaxResponseBodyBytes);
    assert(JsonSyntax(response.body).valid());
}

Snapshot fixture(bool synthetic = false) {
    Snapshot snapshot;
    snapshot.hostId = "reg-win";
    snapshot.processSessionId = "proc-session-1";
    snapshot.testRunId = "run-123";
    snapshot.observedAtUtc = "2026-10-13T10:11:12.123Z";
    snapshot.dataOrigin = synthetic ? "synthetic" : "live";
    snapshot.sequence = std::numeric_limits<std::uint64_t>::max();
    snapshot.monotonicUptimeMs = std::numeric_limits<std::uint64_t>::max();

    Section rtsp;
    rtsp.name = "rtsp";
    rtsp.availability = Availability::Stale;
    Status conn;
    conn.name = "rtsp.connected";
    conn.booleanValue = false;
    conn.textValue = "disconnected";
    conn.unsignedValue = 0;
    conn.availability = Availability::Stale;
    rtsp.statuses.push_back(conn);
    Status arbitrary;
    arbitrary.name = "rtsp.last_error";
    arbitrary.textValue = "rtsp://user:secret@host/reg";
    rtsp.statuses.push_back(arbitrary);
    snapshot.sections.push_back(rtsp);

    Section video;
    video.name = "video";
    video.availability = Availability::Fresh;
    video.dataAgeMs = 0;
    video.lastFrameKey = FrameKey{9007199254740993ULL, std::numeric_limits<std::uint64_t>::max()};
    Metric counter;
    counter.name = "video.decoded_frames_total";
    counter.unit = "frames";
    counter.owner = "reg";
    counter.population = "all_decode_successes";
    counter.clockDomain = "reg_monotonic";
    counter.resetScope = "decoder_session";
    counter.resetId = "reset-9007199254740993";
    counter.kind = MetricKind::Counter;
    counter.availability = Availability::Fresh;
    counter.provenance = Provenance::Measured;
    counter.counter = std::numeric_limits<std::uint64_t>::max();
    counter.sampleCount = std::numeric_limits<std::uint64_t>::max();
    counter.eligibleCount = 0;
    counter.windowStartNs = std::numeric_limits<std::int64_t>::min();
    counter.windowEndNs = std::numeric_limits<std::int64_t>::max();
    counter.windowComplete = false;
    video.metrics.push_back(counter);

    Metric distribution;
    distribution.name = "exact_sync.arrival_wait_matched_only_ms";
    distribution.unit = "ms";
    distribution.owner = "reg";
    distribution.population = "matched_pairs_only";
    distribution.clockDomain = "reg_monotonic";
    distribution.resetScope = "process_session";
    distribution.resetId = "proc-session-1";
    distribution.kind = MetricKind::Distribution;
    distribution.availability = Availability::Stale;
    distribution.provenance = Provenance::Derived;
    distribution.sampleCount = 256;
    distribution.missingCount = 9;
    distribution.timeoutCount = 2;
    distribution.skippedCount = 0;
    distribution.p50 = 24.25;
    distribution.p95 = std::numeric_limits<double>::infinity();
    distribution.p99 = std::numeric_limits<double>::quiet_NaN();
    distribution.quantileRelativeUpperErrorBound = .02;
    distribution.quantilesAreUpperBounds = true;
    video.metrics.push_back(distribution);

    Metric injected;
    injected.name = "rtsp.secret";
    injected.owner = "RTSP://username:password@host";
    injected.unit = "\"},\n\"injection\":true";
    injected.population = "valid";
    injected.clockDomain = "reg_monotonic";
    injected.resetScope = "process_session";
    injected.resetId = "id";
    injected.provenance = Provenance::Estimated;
    injected.value = -12.5;
    video.metrics.push_back(injected);
    snapshot.sections.push_back(video);

    Section cv;
    cv.name = "cv";
    cv.availability = Availability::Unavailable;
    snapshot.sections.push_back(cv);
    Section exact;
    exact.name = "exact_sync";
    exact.availability = Availability::Unsupported;
    snapshot.sections.push_back(exact);
    return snapshot;
}

void verify() {
    auto live = fixture();
    auto snapshot = respond({"GET", "/v1/snapshot"}, live);
    ok(snapshot);
    contains(snapshot.body, "\"stream_epoch\":\"9007199254740993\"");
    contains(snapshot.body, "\"frame_id\":\"18446744073709551615\"");
    contains(snapshot.body, "\"snapshot_sequence\":\"18446744073709551615\"");
    contains(snapshot.body, "\"monotonic_uptime_ms\":\"18446744073709551615\"");
    contains(snapshot.body, "\"counter\":\"18446744073709551615\"");
    contains(snapshot.body, "\"sample_count\":\"18446744073709551615\"");
    contains(snapshot.body, "\"eligible_count\":\"0\"");
    contains(snapshot.body, "\"window_start_ns\":\"-9223372036854775808\"");
    contains(snapshot.body, "\"window_end_ns\":\"9223372036854775807\"");
    contains(snapshot.body, "\"window_complete\":false");
    contains(snapshot.body, "\"boolean_value\":false");
    contains(snapshot.body, "\"unsigned_value\":\"0\"");
    contains(snapshot.body, "\"data_age_ms\":\"0\"");
    contains(snapshot.body, "\"availability\":\"stale\"");
    contains(snapshot.body, "\"availability\":\"unsupported\"");
    contains(snapshot.body, "\"availability\":\"unavailable\"");
    contains(snapshot.body, "\"provenance\":\"derived\"");
    contains(snapshot.body, "\"provenance\":\"estimated\"");
    contains(snapshot.body, "\"population\":\"matched_pairs_only\"");
    contains(snapshot.body, "\"quantiles_are_upper_bounds\":true");
    contains(snapshot.body, "\"quantile_relative_upper_error_bound\":0.02");
    contains(snapshot.body, "\"p95\":null,\"p99\":null");
    contains(snapshot.body, "\"unit\":null,\"owner\":null");
    contains(snapshot.body, "\"text_value\":null");
    excludes(snapshot.body, "RTSP://username");
    excludes(snapshot.body, "user:secret");
    excludes(snapshot.body, "injection");
    excludes(snapshot.body, ":NaN");
    excludes(snapshot.body, ":Infinity");
    assert(snapshot.body == respond({"GET", "/v1/snapshot"}, live).body);

    auto filtered = respond({"GET", "/v1/snapshot?sections=video,rtsp"}, live);
    ok(filtered);
    contains(filtered.body, "\"rtsp\":{");
    contains(filtered.body, "\"video\":{");
    excludes(filtered.body, "\"cv\":{");
    excludes(filtered.body, "\"ui\":{");
    ok(respond({"GET", "/v1/snapshot?sections=ui"}, live));
    contains(respond({"GET", "/v1/snapshot?sections=ui"}, live).body, "\"ui\":{\"availability\":\"unavailable\"");

    for (auto bad : {
        "/v1/snapshot?", "/v1/snapshot?sections=", "/v1/snapshot?sections=video,",
        "/v1/snapshot?sections=,video", "/v1/snapshot?sections=rtsp,rtsp",
        "/v1/snapshot?sections=wat", "/v1/snapshot?sections=video&sections=rtsp",
        "/v1/snapshot?sections=rtsp&extra=1", "/v1/snapshot?sections=vi%64eo",
        "/v1/snapshot?sections=video?x=1", "/v1/snapshot?foo=bar",
        "/v1/status?sections=video", "/healthz?verbose=1", "/openapi.json?x=1"
    }) {
        auto r = respond({"GET", bad}, live);
        ok(r, 400);
        contains(r.body, "\"code\":\"bad_query\"");
        excludes(r.body, bad); // no echo of dangerous raw query
    }
    for (auto method : {"POST", "PUT", "PATCH", "DELETE", "OPTIONS", "HEAD", "get"}) {
        auto r = respond({method, "/v1/snapshot"}, live);
        ok(r, 405);
        contains(r.body, "\"code\":\"method_not_allowed\"");
    }
    ok(respond({"GET", "/v1/series"}, live), 404);
    ok(respond({"GET", "/other?secret"}, live), 404);
    ok(respond({"GET", std::string(8193, 'x')}, live), 413);
    ok(respond({"GET", "/v1/snapshot"}, static_cast<const Snapshot*>(nullptr)), 503);
    ok(respond({"GET", "/v1/status"}, static_cast<const Snapshot*>(nullptr)), 503);
    ok(respond({"GET", "/healthz"}, static_cast<const Snapshot*>(nullptr)));
    ok(respond({"GET", "/v1/capabilities"}, static_cast<const Snapshot*>(nullptr)));
    auto health = respond({"GET", "/healthz"}, live);
    ok(health);
    contains(health.body, "\"alive\":true,\"scope\":\"listener_only\"");
    excludes(health.body, "rtsp");
    excludes(health.body, "video");
    excludes(health.body, "cv");
    auto status = respond({"GET", "/v1/status"}, live);
    ok(status);
    contains(status.body, "\"boolean_value\":false");
    excludes(status.body, "\"metrics\"");
    excludes(status.body, "\"healthy\":true");
    auto capabilities = respond({"GET", "/v1/capabilities"}, live);
    ok(capabilities);
    contains(capabilities.body, "\"rtsp\":\"stale\"");
    contains(capabilities.body, "\"ui\":\"unavailable\"");

    auto synthetic = fixture(true);
    auto syntheticResponse = respond({"GET", "/v1/snapshot"}, synthetic);
    ok(syntheticResponse);
    contains(syntheticResponse.body, "\"data_origin\":\"synthetic\"");
    contains(snapshot.body, "\"data_origin\":\"live\"");
    synthetic.dataOrigin = "bad:untrusted";
    contains(respond({"GET", "/v1/snapshot"}, synthetic).body, "\"data_origin\":null");
    live.hostId = "C:\\\\secrets\\\\auth.env";
    contains(respond({"GET", "/v1/snapshot"}, live).body, "\"host_id\":null");
    live.observedAtUtc = "bad\" timestamp";
    contains(respond({"GET", "/v1/snapshot"}, live).body, "\"observed_at_utc\":null");

    using reg::diagnostics::api::codec::escape_json;
    const auto escaped = escape_json(std::string{"a\"\\\n\r\t\b\f\x01\0", 10});
    assert(escaped == "a\\\"\\\\\\n\\r\\t\\b\\f\\u0001\\u0000");
    assert(escape_json("\xEF\xBF\xBD") == "\xEF\xBF\xBD");
    assert(escape_json("\xFF") == "\\uFFFD");
    assert(escape_json("\xC0\xAF") == "\\uFFFD\\uFFFD");
    assert(escape_json("\xED\xA0\x80") == "\\uFFFD\\uFFFD\\uFFFD");
    assert(escape_json("\xF4\x90\x80\x80") == "\\uFFFD\\uFFFD\\uFFFD\\uFFFD");
    assert(escape_json("\xE2\x98\x83") == "\xE2\x98\x83");

    Snapshot huge = fixture();
    auto& metrics = huge.sections.at(1).metrics;
    const Metric example = metrics.front();
    for (std::size_t i = 0; i < 5000; ++i) metrics.push_back(example);
    auto rejected = respond({"GET", "/v1/snapshot"}, huge);
    ok(rejected, 413);
    contains(rejected.body, "\"code\":\"response_too_large\"");
    ok(respond({"GET", "/v1/snapshot?sections=rtsp"}, huge));

    const auto openapi = respond({"GET", "/openapi.json"}, live);
    ok(openapi);
    contains(openapi.body, "\"openapi\": \"3.1.0\"");
    for (auto path : {"/healthz", "/v1/capabilities", "/v1/status", "/v1/snapshot", "/openapi.json"}) contains(openapi.body, path);
    contains(openapi.body, "\"UInt64String\"");
    contains(openapi.body, "\"Metric\"");
    contains(openapi.body, "\"Provenance\"");
    contains(openapi.body, "\"400\"");
    contains(openapi.body, "\"405\"");
    contains(openapi.body, "\"413\"");
    contains(openapi.body, "\"503\"");

    std::cout << "diagnostic_api_codec_tests: PASS (routes, statuses, uint64, FrameKey, null, NaN, UTF-8, redaction, max body, OpenAPI)\n";
}

void emit(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto snap = fixture(true);
    for (const auto& [name, url] : std::vector<std::pair<std::string, std::string>>{
        {"healthz.json", "/healthz"}, {"capabilities.json", "/v1/capabilities"},
        {"status.json", "/v1/status"}, {"snapshot.json", "/v1/snapshot"},
        {"filtered.json", "/v1/snapshot?sections=rtsp,video"},
        {"openapi.json", "/openapi.json"},
        {"error400.json", "/v1/snapshot?sections=unknown"},
        {"error404.json", "/other"}
    }) {
        const auto r = respond({"GET", url}, snap);
        std::ofstream stream(root / name, std::ios::binary);
        assert(stream.good()); stream << r.body; assert(stream.good());
    }
}
} // namespace

int main(int argc, char** argv) {
    verify();
    if (argc == 3 && std::string_view(argv[1]) == "--emit-fixtures") emit(argv[2]);
}
