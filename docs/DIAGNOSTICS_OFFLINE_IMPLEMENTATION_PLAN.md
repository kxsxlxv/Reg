# Reg: Offline Implementation Plan for Diagnostics and Testbed Intelligence

**Status:** prioritized implementation plan; **not executed**.
**Scope:** work possible without live UNIGINE, MediaMTX RTSP, Jetson or CV inference. Based on feat/netimgui-russian-detr @ 82641a176 and docs/diagnostics-contract-and-agent-guide as of 2026-10-12.
**Goal:** deliver a tested diagnostic/statistical core and agent-readable read-only interface, so later three-host integration requires adapters and physical validation, not architectural redesign.

## Principles

1. Instrument at the owner, compute stats locally, correlate deterministically, ask LLM only targeted questions.
2. Do not simulate a "live successful lab" in production. Synthetic fixtures/test mode are explicitly flagged with data_origin=synthetic; never expose fixture data as real Jetson observations.
3. No diagnostics network/file operations, sorting or serialization in decode/render/CV callbacks.
4. Exact FrameKey = (stream_epoch, frame_id), no timestamp-approximate substitution.
5. Unknown, stale, disabled, failed, never observed and healthy-zero are distinct states.
6. All tests and JSON contracts must run on both Windows and Linux without RTSP/Jetson, ideally without GPU/GUI by splitting core libraries from reg_probe.

## Current baseline (verified)

Already available:
- src/telemetry/TelemetryModel.hpp/.cpp: counters, event ring and sampled 250 ms history with ~5-minute retention; snapshots currently copy vectors under a mutex.
- src/diagnostics/ExactSyncDiagnostics.hpp: per-key counts and P50/P95/P99 from at most 256 successful-match local waiting samples.
- src/diagnostics/FrameTimingLogger: asynchronous per-packet/frame CSV.
- src/video/OverlayFrameBuffer, FrameSynchronizer and MetadataStore: exact frame-key buffering and match/drop semantics.
- MetadataReceiver and Jetson MetadataBridge + loopback tests; Blackbox and replay tests.
- CMake/CTest, Windows and Linux CI, tests for frame sync, metadata bridge, telemetry and restart contract.

Important gaps:
- No live Reg Diagnostic HTTP API, no stable structured event taxonomy, no cross-session analytics engine or typed episode service.
- Current matched-only quantiles do not describe late/never-arriving metadata.
- UI rates computed from history are not yet a shared statistical service.
- No independent headless full-stack diagnostic fixture/driver, so endpoint contract testing would otherwise depend on graphics/runtime.
- No verified physical three-host integration; unit tests cannot establish network/decoder/Jetson timing behaviour.

## Ranked backlog

| Rank | Priority | Work package | Can finish offline? | Suggested effort | Testable exit |
| --- | --- | --- | --- | --- | --- |
| 1 | P0 | Extract a platform-independent diagnostics core library, shared metric registry and injectable clock | Yes | 1–2 dev-days | CTest works without RTSP, GPU context or display |
| 2 | P0 | Implement owner-local stats: rate, frame gaps, durations, count/sum/min/max/mean, mergeable histogram P50/P95/P99, 1/10/60 s windows, reset/freshness | Yes | 2–4 dev-days | Known deterministic vectors, tails and missing/timeout populations pass |
| 3 | P0 | Implement deterministic synthetic scenario runner and reference corpus | Yes | 2–4 dev-days | Fixed seed, virtual clock; replay yields identical traces/assertions |
| 4 | P0 | Introduce lifecycle accounting for Exact Sync and honest delay distributions | Yes | 2–3 dev-days | Late/missing/out-of-order/epoch/eviction cases accounted exactly; old matching invariants unchanged |
| 5 | P1 | Implement versioned read-only Diagnostic API over immutable snapshots; independent headless fake-data service | Yes | 2–4 dev-days | /healthz, /v1/capabilities, /v1/status, /v1/snapshot, /openapi.json match fixtures on loopback |
| 6 | P1 | Typed fault events, debounced transitions and bounded diagnostic episodes | Yes | 3–5 dev-days | Flood of repeated errors creates one bounded episode with counts and evidence_refs |
| 7 | P1 | Experiment manifests, immutable evidence bundle, offline comparisons | Yes | 2–3 dev-days | Saved incident replays with same deterministic verdict; versions/config/time scope preserved |
| 8 | P1 | API/metrics security, stress/fault tests and CI wiring | Yes | 2–3 dev-days | Invalid requests, stale data, disconnects, 64-bit IDs, redaction and overload safe |
| 9 | P2 | Local MCP adapter with bounded overview/stats/episodes/trace tools | Yes | 1–3 dev-days | Codex or test client can investigate synthetic episode without raw logs |
| 10 | P2 | Adaptive CV Overlay Delay Observe offline simulator | Partially | 3–5 dev-days | Observed recommendations compared with Manual on deterministic workload; cannot ship Auto from this |
| 11 | P2 | Telemetry color UX change | Yes (render snapshot/CI) | Hours | CV/Jetson green; Decoder cyan; warning amber stays semantically distinct |

Efforts are coarse relative priorities, not delivery promises. First four jobs provide most algorithmic value and unblock independent agents.

## 1. Extract diagnostics core

Create an independent library (working name reg_diagnostics_core) from the existing domain telemetry components, avoiding direct dependencies on SDL3, Vulkan, ImPlot, FFmpeg, GPU devices or RTSP sockets for analytics. Keep video references as small immutable observations: FrameKey, local monotonic time, session/reset identity, event and counters.

Do not rip out or rewrite working Reg logic wholesale. Introduce narrow interfaces and adapt existing TelemetryModel incrementally. Existing GUI and Blackbox metrics should continue to read authoritative sources. Avoid storing per-frame heavyweight objects in analytics.

Definition of done: Windows and Linux compile and execute all diagnostic tests headlessly; no network or display required, and a test provides fake time deterministically.

## 2. Statistical engine

Represent a metric as a typed definition: name, unit, producer, kind (counter/gauge/duration), sample inclusion rules, reset scope, provenance and freshness. Prefer compact fixed-window or bounded sliding-window accumulators; quantiles require bounded samples or histograms with stated precision. Track count, sum, min, max, mean, P50/P95/P99, denominator, timeout/error/censored counts and number of valid samples.

Rates use delta / local elapsed time across the same generation. Do not average P95s or merge unlike populations. Make explicit that decoder FPS, Raw presents per second and CV-Overlay matches per second measure different boundaries. Include inter-frame spacing and longest no-frame gap, not just FPS.

Test window boundary, empty population, cold start, session restart, counter wrap/reset, epoch change, long idle interval, NaN/Inf and large uint64 values. Specify semantics when there are fewer than 100 observations for P99.

## 3. Synthetic offline scenario runner

Build a deterministic console/CTest driver (e.g. reg_diag_scenarios) using virtual monotonic time and test fixtures. Generate event sequences without rendering, UDP streams or external machines.

Minimum scenarios:
- 60 FPS source, regular 16.67 ms frame spacing, healthy decoder/Raw/CV;
- mean 60 FPS but bursty output with 180 ms gap;
- decoder stall -> reconnect -> resumed frames and changed decoder generation;
- lost source key, source epoch switch, nonmonotonic frame ID;
- CV zero detected objects (valid metadata), missing metadata, late arrival, duplicated and reordered CVM1;
- inference duration increased, queue backlog grows, sent packet not accepted by simulated Reg receiver;
- Overlay delay 50/100/150/200/300 ms; fixed-capacity buffer pressure and metadata drops;
- recorder queue saturation, independent NetImgui disconnect, stale API collector;
- clock offset/drift and counter reset across simulated hosts;
- ambiguous multiple faults and healthy run with coincidental warnings.

Produce tiny golden JSON evidence snapshots and definitive deterministic expectations. Keep source generation pure and seeded; an injected *fault cause* is ground truth for tests, not a runtime causal fact.

## 4. Exact Sync lifecycle model

Augment measurement accounting, not match policy. Every eligible decoded video FrameKey transitions through a bounded lifecycle: decoded, buffered, due, presented_exact, dropped_missing_metadata, dropped_buffer_eviction, dropped_missing_identity (non-eligible), or superseded/expired as precisely defined. CV metadata can arrive before decode, before/after deadline, after drop, with valid zero targets, or never arrive; record these separately.

Derive match coverage with a correct denominator and report late/censored/unobserved populations explicitly. Current matched-only ExactSync P95 remains available under an unambiguous name. Preserve exact-key-only presentation. Test changes to delay deadlines with queued frames before designing Auto.

## 5. Diagnostic API

Use the proposed DIAGNOSTICS_API_SPEC.md v1; freeze a modest first schema instead of implementing every planned route. Loopback-only GET endpoints: /healthz, /v1/capabilities, /v1/status, /v1/snapshot; generate /openapi.json. Implement using a background HTTP worker and immutable copied snapshot. If runtime integration is blocked by Vulkan/RTSP absence, expose the same handler over a small **headless fixture executable** in CI.

Apply auth/redaction/rate/response limits, define typed availability (fresh/stale/unsupported/disabled/error), process/session/build IDs, age, reset semantics and JSON decimal strings for uint64. Never treat a reachable API as proof that the stream is receiving. No arbitrary file reads or changes to runtime configuration.

## 6. Episodes and compact LLM retrieval

Add structured event transitions and a deterministic episode aggregator. For a noisy log source, aggregate duplicate event type, start/end, count, peak/worst observations and a bounded list of evidence refs; retain detailed evidence separately. No continuous raw log subscription tool for models.

Proposed bounded tools:
- overview(5m): <=2 KiB target, top <=5 active/completed episodes;
- metric_stats(metric,window): owner-computed distribution and sample metadata;
- episodes(type,severity,limit): bounded periods with refs;
- compare_windows: deterministic comparison of compatible periods;
- frame_lifecycle(epoch,frame_id): one retained frame trace;
- log_excerpt(episode_id,limit<=20): selected sanitized log lines only;
- evidence_bundle(run_id): manifest and artifact reference rather than inline CSV/video.

Episode correlation is probabilistic/hypothesis-oriented when multiple components are involved; avoid asserting cause merely because events overlap in approximate UTC time.

## 7. Evidence and deterministic acceptance

Every saved run has schema version, test_run_id, scenario seed, configuration, software/build IDs, host identities, clock quality, sessions/epochs, event and metric excerpts, observer errors, injected fault ground truth (test only), and exact expected assertions. Sanitize secrets and preserve input provenance. Separate PASS/FAIL/INCONCLUSIVE from LLM explanations.

A deterministic CI reader must replay a saved synthetic run, compare metrics and reach identical conclusions with any model unplugged. Historical comparisons reject incompatible reset scopes/window populations.

## 8. Offline test and risk plan

Use three layers of test gates:
- Pure unit/property tests (statistics, exact identities, packet/state accounting, virtual time, fault episodes).
- Headless contract/integration tests: JSON responses, OpenAPI, security, offline scenario playback, replay determinism, bounded responses and event memory.
- Runtime/smoke tests on Windows+Linux graphics-capable runners where available: metrics hooked to real Reg data; verify no runtime path regression. These still do **not** establish Jetson or MediaMTX hardware recovery.

Avoid synchronous file I/O, HTTP work or percentile sorting in decode and Vulkan callbacks. Include generated high-rate traces (60/120 fps) and repeated-error floods to check bounded memory and overhead. Set measurable performance thresholds against a local baseline rather than inventing universal CPU/time budgets.

## Deferred until systems are available

- Actual MediaMTX publisher/reader RTP metrics correlation and source-video pacing.
- Jetson hardware inference, queue, optical flow, encoder/decode timestamps and GPU thermal measurements.
- Physical packet-loss/jitter, MediaMTX/UNIGINE restart and real FFmpeg/Vulkan recovery times.
- Clock-quality validation across machines, source SEI to Jetson CVM1 end-to-end delivery and display latency.
- Adaptive CV Overlay Auto, automatic cross-host process management or autonomous disruptive tests.
- Declaring P95/P99 or real-time performance "validated" on Jetson from simulation alone.

## Suggested small implementation PRs

1. reg_diagnostics_core + virtual-clock fixtures + unit tests.
2. Stats metrics windows and histograms + contract fixtures.
3. Synthetic incident runner + exact-sync lifecycle accounting.
4. Headless Diagnostic HTTP API + schema/security tests.
5. Episodes/evidence/compare + offline benchmark.
6. MCP adapter + synthetic demo.
7. Manual/Observe overlay delay simulator and, independently, minor ImGui palette patch.

A PR changing runtime behaviour must update AGENTS.md-driven observability tests/contracts and must not merge unrelated cosmetic/launcher redesign into the diagnostic core. Keep the current launcher design otherwise untouched.

## Project references

- docs/DIAGNOSTICS_API_SPEC.md
- docs/TELEMETRY_ANALYTICS_RESPONSIBILITIES.md
- docs/TESTBED_INTELLIGENCE_VISION.md
- docs/DIAGNOSTICS_AGENT_README.md
- docs/RESTART_RECOVERY_VALIDATION.md
- src/telemetry/TelemetryModel.hpp
- src/diagnostics/ExactSyncDiagnostics.hpp
- src/diagnostics/FrameTimingLogger.hpp
- tests/telemetry_model_tests.cpp
- tests/exact_sync_diagnostics_tests.cpp
- tests/frame_sync_tests.cpp
- tests/jetson_metadata_bridge_tests.cpp
