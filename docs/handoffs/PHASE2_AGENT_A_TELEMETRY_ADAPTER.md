# Phase-2 Agent A — Reg Telemetry → Owner-Local Statistics Adapter

**Mission:** implement the actual C++ adapter from existing Reg telemetry snapshots to the frozen `reg::diagnostics::api::Snapshot` DTO. This is a new task, NOT the already merged Statistics Core #36.
**Base:** `feat/netimgui-russian-detr` *after* the Phase-2 contract PR is merged.
**Suggested branch:** `feat/diag-v1-reg-adapter`. Independent worktree.
**Handoff index:** `docs/handoffs/PHASE2_README.md`.

## Read

`src/diagnostics/api/DiagnosticContract.hpp`, `src/diagnostics/stats/Statistics.hpp`, `src/telemetry/TelemetryModel.hpp/.cpp`, `src/diagnostics/ExactSyncDiagnostics.hpp`, `tests/telemetry_model_tests.cpp`, `tests/diagnostic_statistics_tests.cpp`, `docs/DIAGNOSTICS_API_SPEC.md`, `docs/TELEMETRY_ANALYTICS_RESPONSIBILITIES.md`.

Current `TelemetryModel` stores the latest `Counters` and 250 ms sampled history; `TelemetryModel::snapshot()` copies samples, recent events and targets under a mutex. Existing frontend uses the same counters. Do NOT use target coordinates or log messages as general-purpose API metrics. API must not expose credentials.

## Implement

- New `src/diagnostics/adapter/**`: an offline-testable value adapter accepting **already-copied** `telemetry::Snapshot`, injected local monotonic time, session ID/decoder generation and `dataOrigin`, emitting `api::Snapshot` or named `api::Section` values.
- Use the established `stats::CounterRateWindow` or `FramePacingWindow` for legitimate count-delta/sample-window rate calculations with explicit measured boundaries. Never divide counters by wall uptime, never bridge decoder-session reset, and never average P95. Maintain 1 s, 10 s and 60 s views only where enough valid samples exist. Separate source-unknown vs healthy-zero and mark insufficient/censored windows unavailable.
- Map the **measured Reg-owned counters** to explicit metric names and owner: `rtsp.decoder_sessions_total`, `rtsp.reconnects_total`, `decoder.decoded_frames_total`, `raw.presented_frames_total`, `overlay.presented_decisions_total` (not physical scanout), `cv.packets_total`, `cv.invalid_total`, `cv.duplicates_total`, `exact_sync.matched_pairs_total`, `exact_sync.drop_missing_total`, `exact_sync.evicted_total`, recorder counters and queue depth.
- Status: distinguish `rtspConnected`, `videoSignalPresent`, `cvSignalPresent` from their freshness/age, and `configuredOverlayDelayMs`. `bool false` is legitimate only after observing the source; constructor default zero/false is not proof of failure.
- Exact Sync `matchCoverage` is an exact decision ratio only when denom exists; publish denominators, missing/eviction and metadata censoring flags. Legacy arrival P50/P95/P99 labelled `matched_pairs_only`, last <=256 matched samples; no claim to represent all metadata. Use correct MS and NS conversions.
- Reset boundaries: maintain per-source owner and session; if `decoderSessions` increments, refresh/reset decoder-owned rate series. Avoid making external Jetson inference or MediaMTX RTP loss claims from Reg-only data.
- Distinguish `dataOrigin=synthetic` in test adapters. Never return synthetic as a live provider in production.

## OWN paths

`src/diagnostics/adapter/**` and `tests/diagnostic_telemetry_adapter_tests.cpp`. Own README under adapter if useful. **Do not change** `src/main.cpp`, root `CMakeLists.txt`, `src/telemetry/TelemetryModel.*`, `src/diagnostics/api/DiagnosticContract.hpp`, codec/transport, window render or overlays. Propose a coordinator change if light-weight `countersSnapshot()` from TelemetryModel becomes necessary; do not silently edit shared files.

## Acceptance

- Pure C++23 headless tests using injected virtual monotonic clock + manually generated `telemetry::Snapshot` sequences. Reg build still works on Linux and Windows.
- Fixed counter increments give accurate FPS over known 1/10/60s; equal FPS with different gaps not labelled as equal frame pacing if timing evidence unavailable.
- Counter restarts and decoder session transitions never produce a spurious huge/negative rate. One sample / no samples => null rate.
- Valid zero targets != no metadata, `matched_only` population explicit, incomplete/censored Exact Sync follow-up represented.
- Counter `UINT64_MAX` preserved; output UINT64 remains native until B JSON serializes decimal strings.
- Ensure no locks, heap work or serializing added to decoder/render callbacks. Adapter runs only on a coordinator-owned sampler thread.
- Explicit test of source status unknown vs real false and fresh vs stale.

Submit separate PR, run headless tests, provide commit SHA, branch, CI results, metric table and unresolved coordinator hooks. Do not merge yourself.
