# Phase-2 Agent B — Diagnostic API v1 JSON Router and OpenAPI

**Mission:** implement strict, deterministic HTTP-response JSON formatting and allowlisted v1 route handler **without** writing socket code.
**Suggested branch:** `feat/diag-v1-json-api` from `feat/netimgui-russian-detr` after Phase-2 contract PR. Separate worktree.
**Handoff index:** `docs/handoffs/PHASE2_README.md`.

## Frozen ABI

Read `src/diagnostics/api/DiagnosticContract.hpp` and follow its `Snapshot`, `Section`, `Metric`, `Status`, `HttpRequest` and `HttpResponse`. Do NOT implement your own competing DTO or modify the shared header. Implement a pure entrypoint compatible with:

    namespace reg::diagnostics::api::codec {
      HttpResponse respond(const HttpRequest& request, const Snapshot& snapshot);
    }

No socket objects, globals, runtime polling or TelemetryModel access. Coordinator supplies immutable snapshots. Keep JSON libraries optional: prefer dependency-free C++23 checked escaping/writing; do not add internet-fetched dependencies or runtime GPL libraries.

## Routes and schema

- `GET /healthz`: tiny liveness ONLY; no video/CV-health inference.
- `GET /v1/capabilities`: host role, API version, supported sections/endpoints, freshness/capability states. External Jetson/UNIGINE metrics are unavailable if not instrumented.
- `GET /v1/status`: compact current `Status` fields and their availability; no made-up `healthy=true`.
- `GET /v1/snapshot` with optional `?sections=rtsp,video,cv,exact_sync,recorder,ui`: allowlisted section filter, explicit unsupported/disabled state, bounded response and stable metric names.
- `GET /openapi.json`: valid OpenAPI 3.x document matching actual JSON examples and documented 4xx responses. Source spec `docs/DIAGNOSTICS_API_SPEC.md` describes intended contract.

Required JSON rules:
- **All** uint64 fields, counters, FrameKey stream_epoch/frame_id, snapshot sequence, reset identifiers derived from uint64 are decimal strings.
- Numeric rates, ms durations, ratios can be finite JSON numbers; reject or serialize null for NaN/Inf. Absent optional values -> JSON `null`, never 0.
- Every metric includes `availability`, `unit`, `owner`, `population`, `clock_domain`, `reset_scope`, `reset_id`, `kind`, `provenance`; quantile upper-bound error flagged when present.
- No arbitrary event text, URLs with credentials, env vars, path names or runtime logs in response; escape all control chars including backslash/quotes and invalid text according to chosen UTF-8 policy.
- `Snapshot::dataOrigin` must distinguish synthetic test from live. An API implementation must not pretend unsupported external metrics were measured.
- Restrict allowed endpoints to above; 405 non-GET, 404 unknown path, 400 malformed/duplicate query, 413/4xx oversized request (transport enforces header bytes), 503 unavailable provider if coordinator has no snapshot.
- Response body maximum 1 MiB, ideally much smaller for status/capabilities. Do not silently cut JSON mid-object; return a bounded error instead.
- Make `/v1/status` and `/healthz` unavailable/unknown semantics explicit and independently tested.

## OWN paths

`src/diagnostics/api/codec/**`, `tests/diagnostic_api_codec_tests.cpp`, codec-specific OpenAPI reference under same `codec/**`. Do **not** edit `src/main.cpp`, `CMakeLists.txt`, contract header, `src/diagnostics/adapter/**`, `src/diagnostics/api/transport/**`, old Testbed docs or CI workflows; coordinator owns integration.

## Test matrix

Headless tests with `Snapshot` fixture:
1. Healthy, disconnected and unobserved RTSP/CV: service `healthz` still means only service alive.
2. Section whitelist, empty filter, unknown section, duplicate query params, method rejection.
3. `UINT64_MAX` FrameKey and counters round trip as JSON **strings**; null/missing not 0.
4. JSON escape injection, control chars, newlines, unicode/UTF-8 policy, NaN/Inf.
5. Size bound, unknown/unsupported sections, no data, synthetic flagged.
6. Real JSON parser verifies `/openapi.json` document and representative response payload, or an equivalent reliable isolated validation test.
7. No network, Jetson/RTSP or graphics dependencies; deterministic bytewise snapshots under identical inputs.

Deliver actual C++ implementation and tests + README for the wire schema. PR, commit and test evidence. Do not merge yourself.
