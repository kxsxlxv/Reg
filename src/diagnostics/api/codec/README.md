# Reg Diagnostic API v1 — JSON codec / route layer

**Scope:** pure, synchronous, read-only mapping of the coordinator's immutable
`reg::diagnostics::api::Snapshot` to `HttpResponse`. No networking, threads,
TelemetryModel, GPU, RTSP, filesystem or third-party JSON dependencies.

## API seam

```cpp
#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"

using namespace reg::diagnostics::api;
HttpResponse response = codec::respond(HttpRequest{"GET", "/v1/snapshot?sections=video,cv"}, snapshot);
// When no sampled snapshot exists yet, pass nullptr using the pointer overload:
HttpResponse unavailable = codec::respond(HttpRequest{"GET", "/v1/status"}, static_cast<const Snapshot*>(nullptr));
```

The transport Agent C hands a parsed `HttpRequest` to `respond()`, and copies
`statusCode`, `contentType`, and `body` to HTTP. It must enforce loopback-only
binding, header/request byte limits and timeouts; this codec additionally
rejects targets >8192 bytes and JSON bodies >1048576 bytes. The coordinator
provides snapshot publication/caching (off all time-critical paths). With a
non-null `Snapshot` the codec reports data as provided; **only the coordinator**
can reliably know whether it currently has a valid cached snapshot. Use the
pointer overload with `nullptr` until available, rather than using default-
constructed/zero-valued DTOs as a substitute for absence.

## Wire contract

The implemented routes are `GET /healthz`, `GET /v1/capabilities`,
`GET /v1/status`, `GET /v1/snapshot` (optional `sections=`), and
`GET /openapi.json`. No mutators or arbitrary histories/logs are accepted.
`GET /healthz` is listener liveness **only**, regardless of RTSP, decode or CV.

`/v1/snapshot` yields a stable JSON envelope:

- `schema_version`, `host_role`, `host_id`, `process_session_id`, `test_run_id`,
  `data_origin`, `observed_at_utc`, `snapshot_sequence`, `monotonic_uptime_ms`;
- `sections` is an object indexed by the **six fixed** names `rtsp`, `video`,
  `cv`, `exact_sync`, `recorder`, `ui`. With no filter, all six appear.
  Missing sections appear explicitly as `{availability:"unavailable", ...}`.
  Requested sections are emitted in the fixed canonical order, even when the
  caller's comma-separated filter order differs. Duplicate, empty, unknown,
  percent-encoded or other query parameters are rejected with `400`.
- Each section contains `availability`, `data_age_ms`, `last_frame_key`,
  `metrics` and `statuses`; `/v1/status` has all sections but omits `metrics`.
- Each metric carries `name`, `availability`, `unit`, `owner`, `population`,
  `clock_domain`, `reset_scope`, `reset_id`, `kind`, `provenance` and all optional
  measurement/sample/quantile fields from the shared DTO. The histogram upper
  bound and estimated relative error remain explicit. Counter values, counts,
  `snapshot_sequence`, `monotonic_uptime_ms`, `data_age_ms`, `unsigned_value`,
  frame key components are **decimal JSON strings**, never JSON numbers;
  signed `window_start_ns` and `window_end_ns` are also decimal strings.
  Absent optional values and non-finite numbers are JSON `null`, not zero.
- `data_origin` is either `live`, `synthetic`, or `null` if invalid. Do not
  interpret non-finite quantiles as available evidence; do not combine
  observations across mismatched clock domains, populations or reset IDs.
- The codec preserves availability independently of numeric value, allowing
  fresh/stale/unavailable/unsupported/disabled/error to remain distinguishable.

### Security / UTF-8 policy

The codec intentionally treats DTO labels as *identifiers*, not arbitrary
strings: `name`, owner, population, clock, reset IDs, host IDs and test-run IDs
must be bounded ASCII identifiers (`[A-Za-z0-9_.:-]`, <=128 bytes; units also
allow `/`, `%`, `^`, `*`). Invalid identifiers serialize to `null`, never echoing
raw RTSP URLs, credentials, paths, environment variables, or logs. For
`Metric.reset_id` specifically, the **canonical, case-sensitive syntax** is
`^[A-Za-z0-9_.:-]{1,128}# Reg Diagnostic API v1 — JSON codec / route layer

**Scope:** pure, synchronous, read-only mapping of the coordinator's immutable
`reg::diagnostics::api::Snapshot` to `HttpResponse`. No networking, threads,
TelemetryModel, GPU, RTSP, filesystem or third-party JSON dependencies.

## API seam

```cpp
#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"

using namespace reg::diagnostics::api;
HttpResponse response = codec::respond(HttpRequest{"GET", "/v1/snapshot?sections=video,cv"}, snapshot);
// When no sampled snapshot exists yet, pass nullptr using the pointer overload:
HttpResponse unavailable = codec::respond(HttpRequest{"GET", "/v1/status"}, static_cast<const Snapshot*>(nullptr));
```

The transport Agent C hands a parsed `HttpRequest` to `respond()`, and copies
`statusCode`, `contentType`, and `body` to HTTP. It must enforce loopback-only
binding, header/request byte limits and timeouts; this codec additionally
rejects targets >8192 bytes and JSON bodies >1048576 bytes. The coordinator
provides snapshot publication/caching (off all time-critical paths). With a
non-null `Snapshot` the codec reports data as provided; **only the coordinator**
can reliably know whether it currently has a valid cached snapshot. Use the
pointer overload with `nullptr` until available, rather than using default-
constructed/zero-valued DTOs as a substitute for absence.

## Wire contract

The implemented routes are `GET /healthz`, `GET /v1/capabilities`,
`GET /v1/status`, `GET /v1/snapshot` (optional `sections=`), and
`GET /openapi.json`. No mutators or arbitrary histories/logs are accepted.
`GET /healthz` is listener liveness **only**, regardless of RTSP, decode or CV.

`/v1/snapshot` yields a stable JSON envelope:

- `schema_version`, `host_role`, `host_id`, `process_session_id`, `test_run_id`,
  `data_origin`, `observed_at_utc`, `snapshot_sequence`, `monotonic_uptime_ms`;
- `sections` is an object indexed by the **six fixed** names `rtsp`, `video`,
  `cv`, `exact_sync`, `recorder`, `ui`. With no filter, all six appear.
  Missing sections appear explicitly as `{availability:"unavailable", ...}`.
  Requested sections are emitted in the fixed canonical order, even when the
  caller's comma-separated filter order differs. Duplicate, empty, unknown,
  percent-encoded or other query parameters are rejected with `400`.
- Each section contains `availability`, `data_age_ms`, `last_frame_key`,
  `metrics` and `statuses`; `/v1/status` has all sections but omits `metrics`.
- Each metric carries `name`, `availability`, `unit`, `owner`, `population`,
  `clock_domain`, `reset_scope`, `reset_id`, `kind`, `provenance` and all optional
  measurement/sample/quantile fields from the shared DTO. The histogram upper
  bound and estimated relative error remain explicit. Counter values, counts,
  `snapshot_sequence`, `monotonic_uptime_ms`, `data_age_ms`, `unsigned_value`,
  frame key components are **decimal JSON strings**, never JSON numbers;
  signed `window_start_ns` and `window_end_ns` are also decimal strings.
  Absent optional values and non-finite numbers are JSON `null`, not zero.
- `data_origin` is either `live`, `synthetic`, or `null` if invalid. Do not
  interpret non-finite quantiles as available evidence; do not combine
  observations across mismatched clock domains, populations or reset IDs.
- The codec preserves availability independently of numeric value, allowing
  fresh/stale/unavailable/unsupported/disabled/error to remain distinguishable.

### Security / UTF-8 policy

The codec intentionally treats DTO labels as *identifiers*, not arbitrary
strings: `name`, owner, population, clock, reset IDs, host IDs and test-run IDs
must be bounded ASCII identifiers (`[A-Za-z0-9_.:-]`, <=128 bytes; units also
allow `/`, `%`, `^`, `*`). Invalid identifiers serialize to `null`, never echoing
 (ASCII, 1–128 bytes). For example,
`p-1.decoder:2` and `p-1.exact:7` are valid process/generation-scoped IDs;
`p-1/decoder:2`, filesystem paths, URL-bearing strings, empty IDs and strings
longer than 128 bytes serialize as JSON `null`. In particular, the codec does
**not** substitute delimiters, trim or normalize identifiers: an invalid ID
must be corrected by its data owner, not guessed by the HTTP layer. Rates must
carry the same canonical reset identity as their originating population; a
`null` reset ID makes cross-snapshot counter/rate comparison unsafe.

Status 
`text_value` accepts only a closed, documented-in-source vocabulary of ordinary
state tokens, otherwise `null` (the adapter should use typed boolean/count
and `availability` for the remaining states). UTC timestamps accept only
`YYYY-MM-DDTHH:MM:SS[.digits]Z` format (1–9 fractional digits); invalid ones
become `null`. All rendered text goes through RFC8259 escaping; invalid UTF-8
is replaced with U+FFFD. Error responses have fixed, non-reflective messages.

### Error behavior

| Status | `error.code` | When |
| ------ | ------------ | ---- |
| 400 | `bad_query` | Any malformed/duplicate/disallowed query |
| 404 | `not_found` | Unrecognized path |
| 405 | `method_not_allowed` | Anything other than GET |
| 413 | `request_too_large` | Target exceeds 8192 bytes |
| 413 | `response_too_large` | Serialized JSON exceeds 1 MiB |
| 503 | `snapshot_unavailable` | Null provider snapshot on status/snapshot |

The OpenAPI 3.1 schema is embedded in `OpenApiDocument.hpp`, served bytewise
as `/openapi.json`. `/v1/capabilities` and `/healthz` remain usable with no
snapshot. The transport must not equate a 200 healthz with stream health.

## Offline test command (no root CMake modifications)

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/diagnostics/api/codec/DiagnosticJsonApi.cpp \
  tests/diagnostic_api_codec_tests.cpp -o diagnostic_api_codec_tests
./diagnostic_api_codec_tests
```

Optional fixtures can be emitted with `./diagnostic_api_codec_tests
--emit-fixtures /tmp/reg-codec-fixtures`; parse/validate them against the
`/openapi.json` schema using any draft-2020-12 JSON Schema validator.
The test suite is deterministic, uses no transport and covers route/status
logic, size caps, injection, reset-ID compatibility and path rejection,
null/falsy states, quantile metadata, synthetic origin, 64-bit boundaries, UTF-8 and JSON grammar.

**Observability impact:** read-only exposure of adapter-owned metrics, with no
new measurement source, sampling or reset policy. Every series remains scoped
by owner, population, clock and reset identity. No transport/decoder/render
instrumentation is introduced by this PR.
