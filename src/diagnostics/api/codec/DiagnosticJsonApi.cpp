#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"
#include "diagnostics/api/codec/OpenApiDocument.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace reg::diagnostics::api::codec {
namespace {

constexpr std::array<std::string_view, 6> kSections{
    "rtsp", "video", "cv", "exact_sync", "recorder", "ui"
};
constexpr std::array<std::string_view, 5> kEndpoints{
    "/healthz", "/v1/capabilities", "/v1/status", "/v1/snapshot", "/openapi.json"
};

// A capped incremental JSON sink: it never accumulates a >1MiB response.
// When exceeded, the caller discards the partial bytes and emits a fixed 413.
class Writer {
public:
    explicit Writer(std::size_t limit = kMaxResponseBodyBytes) : limit_(limit) { body_.reserve(2048); }
    void raw(std::string_view text) {
        if (overflow_ || text.size() > limit_ - body_.size()) { overflow_ = true; return; }
        body_.append(text);
    }
    void quoted(std::string_view text) {
        raw("\"");
        for (std::size_t i = 0; i < text.size() && !overflow_;) {
            const auto ch = static_cast<unsigned char>(text[i]);
            if (ch == '"') { raw("\\\""); ++i; continue; }
            if (ch == '\\') { raw("\\\\"); ++i; continue; }
            if (ch <= 0x1fU || ch == 0x7fU) {
                switch (ch) {
                    case '\b': raw("\\b"); break;
                    case '\f': raw("\\f"); break;
                    case '\n': raw("\\n"); break;
                    case '\r': raw("\\r"); break;
                    case '\t': raw("\\t"); break;
                    default: {
                        constexpr char hex[] = "0123456789abcdef";
                        char sequence[] = {'\\', 'u', '0', '0', hex[ch >> 4U], hex[ch & 15U]};
                        raw(std::string_view{sequence, sizeof(sequence)});
                    }
                }
                ++i;
                continue;
            }
            if (ch < 0x80U) { raw(text.substr(i, 1)); ++i; continue; }

            // RFC 3629: reject overlong forms, surrogate halves and > U+10FFFF.
            std::size_t n = 0;
            if (ch >= 0xc2U && ch <= 0xdfU) n = 2;
            else if (ch >= 0xe0U && ch <= 0xefU) n = 3;
            else if (ch >= 0xf0U && ch <= 0xf4U) n = 4;
            bool valid = n != 0 && n <= text.size() - i;
            if (valid) {
                for (std::size_t j = 1; j < n; ++j) {
                    const auto c = static_cast<unsigned char>(text[i + j]);
                    if (c < 0x80U || c > 0xbfU) { valid = false; break; }
                }
            }
            if (valid) {
                const auto c1 = static_cast<unsigned char>(text[i + 1]);
                if ((ch == 0xe0U && c1 < 0xa0U) ||
                    (ch == 0xedU && c1 >= 0xa0U) ||
                    (ch == 0xf0U && c1 < 0x90U) ||
                    (ch == 0xf4U && c1 > 0x8fU)) valid = false;
            }
            if (valid) { raw(text.substr(i, n)); i += n; }
            else { raw("\\uFFFD"); ++i; }
        }
        raw("\"");
    }
    void field(std::string_view name) { quoted(name); raw(":"); }
    void boolean(bool v) { raw(v ? "true" : "false"); }
    void number(const std::optional<double>& v) {
        if (!v || !std::isfinite(*v)) { raw("null"); return; }
        char digits[64]{};
        const auto [end, error] = std::to_chars(digits, digits + sizeof(digits), *v);
        if (error == std::errc{}) raw(std::string_view{digits, static_cast<std::size_t>(end - digits)});
        else raw("null");
    }
    void u64(const std::optional<std::uint64_t>& v) {
        if (!v) { raw("null"); return; }
        char digits[32]{};
        auto [end, error] = std::to_chars(digits, digits + sizeof(digits), *v);
        if (error == std::errc{}) quoted({digits, static_cast<std::size_t>(end - digits)});
        else raw("null");
    }
    void u64(std::uint64_t v) { u64(std::optional<std::uint64_t>{v}); }
    void i64(const std::optional<std::int64_t>& v) {
        if (!v) { raw("null"); return; }
        char digits[32]{};
        auto [end, error] = std::to_chars(digits, digits + sizeof(digits), *v);
        if (error == std::errc{}) quoted({digits, static_cast<std::size_t>(end - digits)});
        else raw("null");
    }
    void optional_bool(const std::optional<bool>& v) { if (v) boolean(*v); else raw("null"); }
    [[nodiscard]] bool overflow() const { return overflow_; }
    [[nodiscard]] std::string finish() { return std::move(body_); }
private:
    std::string body_;
    std::size_t limit_;
    bool overflow_ = false;
};

bool ascii_token(std::string_view value, bool unit = false) {
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') continue;
        if (c == ':' && !unit) continue;
        if (unit && (c == '/' || c == '%' || c == '^' || c == '*')) continue;
        return false;
    }
    return true;
}
void label(Writer& w, std::string_view v, bool unit = false) {
    // Telemetry labels and IDs are structured identifiers, never free text.
    // Null on invalid characters rather than reflecting URLs, paths or logs.
    if (ascii_token(v, unit)) w.quoted(v);
    else w.raw("null");
}
void optional_label(Writer& w, const std::optional<std::string>& value) {
    if (value) label(w, *value);
    else w.raw("null");
}
void status_text(Writer& w, const std::optional<std::string>& value) {
    // Status text is a closed vocabulary; arbitrary runtime error messages,
    // file paths, user-supplied URLs, environment strings are never relayed.
    constexpr std::array<std::string_view, 27> tokens{
        "active", "inactive", "connected", "disconnected", "connecting",
        "receiving", "streaming", "stopped", "starting", "running",
        "idle", "waiting", "paused", "ready", "enabled", "disabled",
        "fresh", "stale", "unavailable", "unsupported", "unknown",
        "error", "failed", "recovering", "no_signal", "ok", "unobserved"
    };
    if (value && std::find(tokens.begin(), tokens.end(), *value) != tokens.end()) w.quoted(*value);
    else w.raw("null");
}
void timestamp(Writer& w, std::string_view date) {
    // Accepted UTC RFC3339 subset: YYYY-MM-DDTHH:MM:SS[.1-9 digits]Z.
    bool ok = date.size() >= 20 && date.size() <= 30;
    if (ok) {
        for (std::size_t i = 0; i < date.size(); ++i) {
            const bool separator = (i == 4 || i == 7 || i == 10 || i == 13 || i == 16);
            const char expected = i == 10 ? 'T' : (i == 4 || i == 7 ? '-' : ':');
            if (separator) { if (date[i] != expected) { ok = false; break; } }
            else if (i == date.size() - 1) { if (date[i] != 'Z') { ok = false; break; } }
            else if (i == 19 && date.size() > 20) { if (date[i] != '.') { ok = false; break; } }
            else if (date[i] < '0' || date[i] > '9') { ok = false; break; }
        }
    }
    if (ok && date.size() != 20 && date.size() < 22) ok = false;
    if (ok) w.quoted(date); else w.raw("null");
}
std::string_view availability(Availability v) {
    switch (v) {
    case Availability::Fresh: return "fresh";
    case Availability::Stale: return "stale";
    case Availability::Unavailable: return "unavailable";
    case Availability::Unsupported: return "unsupported";
    case Availability::Disabled: return "disabled";
    case Availability::Error: return "error";
    }
    return "unavailable";
}
std::string_view provenance(Provenance v) {
    switch (v) {
    case Provenance::Measured: return "measured";
    case Provenance::Derived: return "derived";
    case Provenance::External: return "external";
    case Provenance::Estimated: return "estimated";
    }
    return "estimated";
}
std::string_view kind(MetricKind v) {
    switch (v) {
    case MetricKind::Counter: return "counter";
    case MetricKind::Gauge: return "gauge";
    case MetricKind::Rate: return "rate";
    case MetricKind::Distribution: return "distribution";
    }
    return "gauge";
}
const Section* get_section(const Snapshot* snapshot, std::string_view name) {
    if (!snapshot) return nullptr;
    const auto it = std::find_if(snapshot->sections.begin(), snapshot->sections.end(),
                                 [name](const Section& v) { return v.name == name; });
    return it == snapshot->sections.end() ? nullptr : &*it;
}
void frame_key(Writer& w, const std::optional<FrameKey>& key) {
    if (!key) { w.raw("null"); return; }
    w.raw("{"); w.field("stream_epoch"); w.u64(key->streamEpoch);
    w.raw(","); w.field("frame_id"); w.u64(key->frameId); w.raw("}");
}
void status(Writer& w, const Status& v) {
    w.raw("{"); w.field("name"); label(w, v.name);
    w.raw(","); w.field("availability"); w.quoted(availability(v.availability));
    w.raw(","); w.field("boolean_value"); w.optional_bool(v.booleanValue);
    w.raw(","); w.field("text_value"); status_text(w, v.textValue);
    w.raw(","); w.field("unsigned_value"); w.u64(v.unsignedValue);
    w.raw("}");
}
void metric(Writer& w, const Metric& v) {
    w.raw("{"); w.field("name"); label(w, v.name);
    w.raw(","); w.field("availability"); w.quoted(availability(v.availability));
    w.raw(","); w.field("unit"); label(w, v.unit, true);
    w.raw(","); w.field("owner"); label(w, v.owner);
    w.raw(","); w.field("population"); label(w, v.population);
    w.raw(","); w.field("clock_domain"); label(w, v.clockDomain);
    w.raw(","); w.field("reset_scope"); label(w, v.resetScope);
    w.raw(","); w.field("reset_id"); label(w, v.resetId);
    w.raw(","); w.field("kind"); w.quoted(kind(v.kind));
    w.raw(","); w.field("provenance"); w.quoted(provenance(v.provenance));
    w.raw(","); w.field("counter"); w.u64(v.counter);
    w.raw(","); w.field("value"); w.number(v.value);
    w.raw(","); w.field("sample_count"); w.u64(v.sampleCount);
    w.raw(","); w.field("eligible_count"); w.u64(v.eligibleCount);
    w.raw(","); w.field("missing_count"); w.u64(v.missingCount);
    w.raw(","); w.field("timeout_count"); w.u64(v.timeoutCount);
    w.raw(","); w.field("skipped_count"); w.u64(v.skippedCount);
    w.raw(","); w.field("window_start_ns"); w.i64(v.windowStartNs);
    w.raw(","); w.field("window_end_ns"); w.i64(v.windowEndNs);
    w.raw(","); w.field("window_complete"); w.optional_bool(v.windowComplete);
    w.raw(","); w.field("min"); w.number(v.min);
    w.raw(","); w.field("max"); w.number(v.max);
    w.raw(","); w.field("mean"); w.number(v.mean);
    w.raw(","); w.field("p50"); w.number(v.p50);
    w.raw(","); w.field("p95"); w.number(v.p95);
    w.raw(","); w.field("p99"); w.number(v.p99);
    w.raw(","); w.field("quantile_relative_upper_error_bound"); w.number(v.quantileRelativeUpperErrorBound);
    w.raw(","); w.field("quantiles_are_upper_bounds"); w.boolean(v.quantilesAreUpperBounds);
    w.raw("}");
}
void section(Writer& w, const Section* v, bool include_metrics) {
    w.raw("{"); w.field("availability"); w.quoted(v ? availability(v->availability) : "unavailable");
    w.raw(","); w.field("data_age_ms"); if (v) w.u64(v->dataAgeMs); else w.raw("null");
    w.raw(","); w.field("last_frame_key"); if (v) frame_key(w, v->lastFrameKey); else w.raw("null");
    if (include_metrics) {
        w.raw(","); w.field("metrics"); w.raw("[");
        if (v) for (std::size_t i = 0; i < v->metrics.size() && !w.overflow(); ++i) {
            if (i) w.raw(",");
            metric(w, v->metrics[i]);
        }
        w.raw("]");
    }
    w.raw(","); w.field("statuses"); w.raw("[");
    if (v) for (std::size_t i = 0; i < v->statuses.size() && !w.overflow(); ++i) {
        if (i) w.raw(",");
        status(w, v->statuses[i]);
    }
    w.raw("]}");
}

HttpResponse response_error(int code, std::string_view slug, std::string_view description) {
    Writer w(1024); w.raw("{\"error\":{\"code\":"); w.quoted(slug);
    w.raw(",\"message\":"); w.quoted(description); w.raw("}}");
    return {code, "application/json; charset=utf-8", w.finish()};
}
HttpResponse finish_response(Writer& w) {
    if (w.overflow()) return response_error(413, "response_too_large", "Diagnostic response exceeds 1 MiB");
    return {200, "application/json; charset=utf-8", w.finish()};
}

std::optional<std::array<bool, kSections.size()>> parse_sections(std::string_view query) {
    std::array<bool, kSections.size()> selected{};
    if (query.empty()) { selected.fill(true); return selected; }
    if (!query.starts_with("sections=")) return std::nullopt;
    auto data = query.substr(sizeof("sections=") - 1);
    if (data.empty()) return std::nullopt;
    while (!data.empty()) {
        const auto comma = data.find(',');
        const auto item = data.substr(0, comma);
        const auto it = std::find(kSections.begin(), kSections.end(), item);
        if (it == kSections.end()) return std::nullopt;
        const auto idx = static_cast<std::size_t>(it - kSections.begin());
        if (selected[idx]) return std::nullopt;
        selected[idx] = true;
        if (comma == std::string_view::npos) break;
        data.remove_prefix(comma + 1);
        if (data.empty()) return std::nullopt;
    }
    return selected;
}

void envelope(Writer& w, const Snapshot& v) {
    w.raw("{"); w.field("schema_version"); label(w, v.schemaVersion);
    w.raw(","); w.field("host_role"); label(w, v.hostRole);
    w.raw(","); w.field("host_id"); label(w, v.hostId);
    w.raw(","); w.field("process_session_id"); label(w, v.processSessionId);
    w.raw(","); w.field("test_run_id"); optional_label(w, v.testRunId);
    w.raw(","); w.field("data_origin");
    if (v.dataOrigin == "live" || v.dataOrigin == "synthetic") w.quoted(v.dataOrigin);
    else w.raw("null");
    w.raw(","); w.field("observed_at_utc"); timestamp(w, v.observedAtUtc);
    w.raw(","); w.field("snapshot_sequence"); w.u64(v.sequence);
    w.raw(","); w.field("monotonic_uptime_ms"); w.u64(v.monotonicUptimeMs);
}

} // namespace

std::string escape_json(std::string_view v) {
    Writer w; w.quoted(v);
    if (w.overflow()) return {}; // strings >1MiB are not exposed
    auto s = w.finish();
    if (s.size() < 2) return {};
    return s.substr(1, s.size() - 2);
}

HttpResponse respond(const HttpRequest& request, const Snapshot& snapshot) {
    return respond(request, &snapshot);
}

HttpResponse respond(const HttpRequest& request, const Snapshot* snapshot) {
    // A target is path + optional query, never a full URL, fragment or body.
    if (request.target.size() > kMaxRequestTargetBytes)
        return response_error(413, "request_too_large", "Request target exceeds 8 KiB");
    if (request.method != "GET")
        return response_error(405, "method_not_allowed", "Only GET is supported");
    const auto mark = request.target.find('?');
    const std::string_view path{request.target.data(), mark == std::string::npos ? request.target.size() : mark};
    const std::string_view query = mark == std::string::npos ? std::string_view{} :
                                   std::string_view(request.target).substr(mark + 1);
    if (path != "/healthz" && path != "/v1/capabilities" && path != "/v1/status" &&
        path != "/v1/snapshot" && path != "/openapi.json")
        return response_error(404, "not_found", "Unknown endpoint");
    if (mark != std::string::npos && (path != "/v1/snapshot" || query.empty()))
        return response_error(400, "bad_query", "Invalid query parameters");

    if (path == "/openapi.json") {
        const auto spec = openapi_document();
        if (spec.size() > kMaxResponseBodyBytes)
            return response_error(413, "response_too_large", "Diagnostic response exceeds 1 MiB");
        return {200, "application/json; charset=utf-8", std::string(spec)};
    }
    if (path == "/healthz")
        return {200, "application/json; charset=utf-8", "{\"service\":\"reg-diagnostics\",\"alive\":true,\"scope\":\"listener_only\"}"};
    if (path == "/v1/capabilities") {
        Writer w; w.raw("{\"api_version\":\"1.0\",\"host_role\":");
        if (snapshot) label(w, snapshot->hostRole); else w.quoted("reg");
        w.raw(",\"data_origin\":");
        if (snapshot && (snapshot->dataOrigin == "live" || snapshot->dataOrigin == "synthetic"))
            w.quoted(snapshot->dataOrigin);
        else w.raw("null");
        w.raw(",\"endpoints\":[");
        for (std::size_t i = 0; i < kEndpoints.size(); ++i) {
            if (i) w.raw(",");
            w.quoted(kEndpoints[i]);
        }
        w.raw("],\"sections\":{");
        for (std::size_t i = 0; i < kSections.size(); ++i) {
            if (i) w.raw(",");
            w.field(kSections[i]);
            const auto* section = get_section(snapshot, kSections[i]);
            w.quoted(section ? availability(section->availability) : "unavailable");
        }
        w.raw("}}"); return finish_response(w);
    }
    if (path == "/v1/snapshot") {
        const auto selected = parse_sections(query);
        if (!selected) return response_error(400, "bad_query", "Invalid query parameters");
        if (!snapshot) return response_error(503, "snapshot_unavailable", "Diagnostic snapshot unavailable");
        Writer w; envelope(w, *snapshot); w.raw(",\"sections\":{");
        bool first = true;
        for (std::size_t i = 0; i < kSections.size() && !w.overflow(); ++i) {
            if (!(*selected)[i]) continue;
            if (!first) w.raw(",");
            first = false;
            w.field(kSections[i]); section(w, get_section(snapshot, kSections[i]), true);
        }
        w.raw("}}"); return finish_response(w);
    }
    if (!snapshot) return response_error(503, "snapshot_unavailable", "Diagnostic snapshot unavailable");
    Writer w; envelope(w, *snapshot); w.raw(",\"sections\":{");
    for (std::size_t i = 0; i < kSections.size() && !w.overflow(); ++i) {
        if (i) w.raw(",");
        w.field(kSections[i]);
        section(w, get_section(snapshot, kSections[i]), false);
    }
    w.raw("}}"); return finish_response(w);
}

} // namespace reg::diagnostics::api::codec
