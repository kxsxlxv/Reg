#pragma once

// Reg Diagnostics API v1 — cross-component, immutable-by-convention DTO.
// Deliberately independent of media/GPU/network/JSON libraries.
// Serialization, collection, and HTTP transport are separate layers.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace reg::diagnostics::api {

enum class Availability : std::uint8_t {
    Fresh, Stale, Unavailable, Unsupported, Disabled, Error
};
enum class Provenance : std::uint8_t {
    Measured, Derived, External, Estimated
};
enum class MetricKind : std::uint8_t {
    Counter, Gauge, Rate, Distribution
};

// Never serialize these unsigned 64-bit values as JSON numbers: emit decimal strings.
struct FrameKey {
    std::uint64_t streamEpoch{};
    std::uint64_t frameId{};
};

// A single metric population with explicit producer/clock/reset boundaries.
// A null value remains null: unavailable != zero. No fabricated Jetson numbers.
struct Metric {
    std::string name;
    std::string unit;
    std::string owner;
    std::string population;
    std::string clockDomain;
    std::string resetScope;
    std::string resetId;
    MetricKind kind{MetricKind::Gauge};
    Availability availability{Availability::Unavailable};
    Provenance provenance{Provenance::Measured};

    // Counter uses C++ uint64, JSON decimal string. Gauge/rate uses JSON number.
    std::optional<std::uint64_t> counter;
    std::optional<double> value;
    std::optional<std::uint64_t> sampleCount;
    std::optional<std::uint64_t> eligibleCount;
    std::optional<std::uint64_t> missingCount;
    std::optional<std::uint64_t> timeoutCount;
    std::optional<std::uint64_t> skippedCount;
    std::optional<std::int64_t> windowStartNs;
    std::optional<std::int64_t> windowEndNs;
    std::optional<bool> windowComplete;

    // Optional distribution summaries only when supported by the population.
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> mean;
    std::optional<double> p50;
    std::optional<double> p95;
    std::optional<double> p99;
    std::optional<double> quantileRelativeUpperErrorBound;
    bool quantilesAreUpperBounds{false};
};

// Health, connection, and activity are separate observations. For optional
// boolean/string status values, absence is NOT equivalent to false or empty.
struct Status {
    std::string name;
    Availability availability{Availability::Unavailable};
    std::optional<bool> booleanValue;
    std::optional<std::string> textValue;
    std::optional<std::uint64_t> unsignedValue;
};

struct Section {
    std::string name;
    Availability availability{Availability::Unavailable};
    std::optional<std::uint64_t> dataAgeMs;
    std::optional<FrameKey> lastFrameKey;
    std::vector<Metric> metrics;
    std::vector<Status> statuses;
};

// A single bounded immutable copy read off the time-critical paths.
struct Snapshot {
    std::string schemaVersion{"1.0"};
    std::string hostRole{"reg"};
    std::string hostId;               // opaque, not credentials or raw paths
    std::string processSessionId;     // resets on process restart
    std::optional<std::string> testRunId;
    std::string dataOrigin{"live"};  // synthetic only in explicit offline test mode
    std::string observedAtUtc;        // UTC publication time; not for cross-host latency
    std::uint64_t sequence{};
    std::optional<std::uint64_t> monotonicUptimeMs;
    std::vector<Section> sections;
};

struct HttpRequest {
    std::string method; // GET supported in v1; other methods explicitly rejected
    std::string target; // raw path+query, bounded by transport
};
struct HttpResponse {
    int statusCode{200};
    std::string contentType{"application/json; charset=utf-8"};
    std::string body;
};

} // namespace reg::diagnostics::api
