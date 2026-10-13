# Phase 2 — Reg Runtime Telemetry and Diagnostic API v1

**Status:** executable base types + handoff pack; network API not yet implemented.
**Repository:** kxsxlxv/Reg. Target integration branch: `feat/netimgui-russian-detr`.
**Baseline before Phase 2:** `15abae374f4990fbdad4d80f9a298a009cf1ba1a` — offline core and all P0 tests integrated (#35, #36, #37, #38).
**Contract PR:** the change introducing `src/diagnostics/api/DiagnosticContract.hpp` and this handoff pack.
**Coordinator:** this original project conversation. No implicit context shared with other agent chats. No Codex ChatGPT Bridge requirement.

## Goal and non-goals

Deliver a real **Reg-local, read-only Diagnostic API v1** consuming true, owner-local TelemetryModel counters and exact-sync observer state. Preserve all current Reg frame-decode/render/RTSP policies. Measure locally; do not infer Jetson clocks, UDP packet loss or physical Vulkan scanout. No MediaMTX/Jetson exporters, root-cause LLM, live frame traces or Adaptive Delay Auto in Phase 2.

The offline-generated fixtures must never be served as live measurements. Headless fake-data mode is **test-only** and explicitly labelled synthetic. Runtime API must distinguish service liveness from stream health. Avoid JSON/HTTP/locks/sorting on frame and CV callbacks.

## Shared ABI — already created by coordinator

Read `src/diagnostics/api/DiagnosticContract.hpp` before coding. It defines `Availability`, `Provenance`, `MetricKind`, `Metric`, `Status`, `Section`, `Snapshot`, `HttpRequest` and `HttpResponse`. This is an explicitly versioned **v1 DTO seam**, not a suggestion to invent an alternative schema. It has no SDL/Vulkan/FFmpeg/network dependency and has a new `diagnostic_api_contract` test.

Semantics:
- JSON `uint64_t` values, IDs, frame epoch and frame ID use **decimal strings**, never JSON doubles. `FrameKey` always has BOTH fields.
- `std::nullopt` is not zero, and availability is separate from current numeric value.
- Metrics have name, unit, owner, sample population, clock domain, reset scope/id, metric kind, provenance, freshness, optional window boundaries and coverage/quantile error metadata.
- `Metric::counter` is integer; `Metric::value` is a finite bounded floating-point gauge/rate. A consumer never chooses a fake zero when `std::nullopt`.
- `Snapshot::dataOrigin=\"live\"` is acceptable only for real Reg telemetry. Tests set `synthetic` explicitly. `observedAtUtc` is response publication time, NOT a comparable host-monotonic timestamp.
- Service `healthz` means listener alive only; no inference of healthy RTSP/CV.
- Unknown external Jetson, UNIGINE and MediaMTX metrics remain `Unsupported`/`Unavailable`, never invented from Reg counters.
- `presentedExactTotal` is exact matching **decision**, not physical swapchain presentation success. Matched-only wait percentiles remain biased/successful-match-only and must be named as such.

## Parallel agents — separate branches and worktrees

| Chat | Owns | Handoff | Suggested branch |
| --- | --- | --- | --- |
| A — Reg Telemetry Adapter | `src/diagnostics/adapter/**`, `tests/diagnostic_telemetry_adapter_tests.cpp` | `PHASE2_AGENT_A_TELEMETRY_ADAPTER.md` | `feat/diag-v1-reg-adapter` |
| B — JSON codec/router/OpenAPI | `src/diagnostics/api/codec/**`, `tests/diagnostic_api_codec_tests.cpp`, codec-specific schema docs | `PHASE2_AGENT_B_JSON_API.md` | `feat/diag-v1-json-api` |
| C — Loopback HTTP transport | `src/diagnostics/api/transport/**`, `tests/diagnostic_http_transport_tests.cpp` | `PHASE2_AGENT_C_LOOPBACK_TRANSPORT.md` | `feat/diag-v1-loopback-http` |
| Coordinator — production wiring/CI | `src/main.cpp`, root `CMakeLists.txt`, app CLI/config, `.github/workflows/**`, shared contract, integration acceptance | This chat | `integration/diag-v1-runtime` later |

Agent work products are **actual code plus executable offline tests**, not planning-only PRs. Changes to common DTO, root CMake, main.cpp, TelemetryModel's public API or other agents' directories require coordinator review; do not edit them in an agent PR. Do not start three agents in one Git worktree.

Start new agent branches from the same `feat/netimgui-russian-detr` tip **after the contract PR is merged**. If starting earlier, branch from the contract PR's head into separate feature branches and clearly label temporarily stacked PRs; no source code added to the contract documentation branch other than coordinator-owned DTO/test.

## Frozen component seams

**A adapter:** constructs a value-semantic `api::Snapshot` from a sampled `reg::telemetry::Snapshot`, supplied monotonic `now`, stable process/session identity and explicit `dataOrigin`. It uses `reg::diagnostics::stats` in the off-critical path. It does *not* publish JSON or open sockets. Sampling is at most about 1 Hz by coordinator initially, to avoid heavy history copies on rendering callbacks.

**B route/JSON:** pure `api::HttpResponse respond(const api::HttpRequest&, const api::Snapshot&)` (or simple equivalent free function declared in codec headers) implementing the allowlisted v1 paths using DTOs. No sockets or direct TelemetryModel reads. Prefer a pure function over duplicate background state.

**C transport:** `LoopbackHttpServer` with `using Handler = std::function<api::HttpResponse(const api::HttpRequest&)>;` supplied at construction or start. Uses only the shared request/response ABI; does not parse DTOs, metrics or JSON. Owns port/bind/thread lifetime.

The coordinator connects C handler -> B responder -> A immutable sampled Snapshot with one bounded, low-priority sampling thread/cache. Avoid allowing HTTP request handling to cause `TelemetryModel::snapshot()` copies or 1/10/60s histogram computation on a time-critical callback.

## HTTP v1 acceptance

Mandatory GET `/healthz`, `/v1/capabilities`, `/v1/status`, `/v1/snapshot?sections=rtsp,video,cv,exact_sync,recorder,ui`, `/openapi.json`. **Default listener disabled** until explicit opt-in via coordinator-owned runtime CLI, and when enabled bind exactly `127.0.0.1` (not `0.0.0.0`). Maximum request header 8 KiB, body response <=1 MiB, small bounded concurrency, read timeout <=2s, reject non-GET. No arbitrary file paths, remote shell, API mutators or raw unbounded logs. Malformed query = 4xx with redacted error. Unsupported sections return availability state.

HTTP JSON definitions and code must agree and be regression-tested; changes to the DTO or external schema require coordinator approval. Cross-platform Windows MSYS2 UCRT64 + Linux must compile and test without Jetson or RTSP.

## Integration and acceptance order

1. Contract PR accepted/merged; agents A, B and C start in separate chats.
2. Merge A, B and C after independent review and Windows/Linux CI; order is flexible because component seams do not share code.
3. Coordinator integrates `src/main.cpp`, opt-in CLI/config, host/process ID and UTC stamping, offline read-only fake provider, headless API contract/loopback tests and end-to-end Reg Counter -> JSON -> HTTP acceptance.
4. Prove localhost-only binding, GET-only, content size bounds, malformed request resistance, no leaked credentials or raw RTSP URL and service-health != video-health.
5. Measure runtime overhead and Jetson/RTSP behaviour **later** on hardware; offline CI cannot validate the real pipeline.

See `docs/DIAGNOSTICS_API_SPEC.md` and `docs/TELEMETRY_ANALYTICS_RESPONSIBILITIES.md` for product intent. The ABI in `DiagnosticContract.hpp` and this handoff take precedence for Phase-2 integration details when older design examples diverge.
