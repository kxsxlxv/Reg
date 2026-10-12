# Handoff A — Reg Headless Statistics Core

**Assignment:** independent reusable diagnostics/statistics library.
**Suggested branch:** feat/diag-stats-core.
**Start:** from current runtime integration ref *after* docs PR #33 merges, or the documentation branch as a temporary stacked base (see docs/handoffs/README.md).
**Priority:** P0; start immediately. No Jetson, UNIGINE, RTSP stream or Vulkan runtime required.
**Your scope is source code and tests, not a planning-only exercise.**

## Essential background

Reg C++23/SDL3/FFmpeg/Vulkan has src/telemetry/TelemetryModel.hpp/.cpp with mutable Counters, 250 ms sampled rate history, five-minute retention and event ring. UI calculates separate decoder/Raw/CV rates from those samples. src/diagnostics/ExactSyncDiagnostics.hpp maintains matching counters and matched-only P50/P95/P99 over a bounded successful-match ring. Existing tests: tests/telemetry_model_tests.cpp, tests/exact_sync_diagnostics_tests.cpp. CMakeLists.txt builds several independent tests.

Preserve Reg invariants in docs/MVP_ARCHITECTURE.md: no GPU-to-CPU video copies, no software decode fallback, no Raw frame FIFO, Overlay only matched full FrameKey. Do not introduce network/disk/JSON/sorting work into critical callbacks.

## Task

Create a platform-independent statistics library (working name reg_diagnostics_core) with a small typed registry and/or strongly typed observation schema:
- metric identity/name, metric kind, unit, owner, observation time/local clock, reset/session scope, sample population, freshness/availability state;
- robust counter deltas and rates across windows with correct zero-time/first-sample/reset behaviour;
- duration aggregates count/sum/min/max/average, P50/P95/P99 with documented algorithm/accuracy and bounded memory;
- frame interval distribution, longest inter-frame gap, missing/timeout/skipped categories separate from successful samples;
- 1 s, 10 s, 60 s and specified experiment windows, with explicit sample_count and window boundaries;
- mergeable histograms or sufficient statistics where compatible. NEVER average P95 across windows or hosts. Explicitly reject incompatible populations or window/reset domains;
- deterministic time injection for tests; C++23, Windows/Linux portable, no SDL/Vulkan/FFmpeg runtime dependency for core tests.

Choose the simplest correct bounded implementation; exact quantiles on a bounded diagnostic ring are acceptable for an individual unmerged stream, but document why merged histograms would need an additional implementation. Prefer efficient O(1) capture cost and background aggregation. Handle NaN, Infinity, large durations/counters, empty samples, insufficient-sample tail estimates, counter wrap/reset and negative/invalid time differences.

## File ownership

OWN:
- New src/diagnostics/stats/** (or src/telemetry/stats/** if justified before coding).
- New tests/diagnostic_statistics_tests.cpp and optional new fixture vectors under tests/fixtures/stats/**.
- Necessary isolated target additions to root CMakeLists.txt.

MUST NOT MODIFY without coordinator approval:
- src/diagnostics/ExactSyncDiagnostics.hpp (owned by C);
- tests/exact_sync_diagnostics_tests.cpp (owned by C);
- tools/diagnostic_scenarios/** and tests/fixtures/diagnostics/** (owned by B);
- src/main.cpp, OverlayFrameBuffer/FrameSynchronizer, Launcher/GUI, agent docs or HTTP server;
- existing TelemetryModel interfaces in a way that breaks UI/other clients. At this stage prefer adapter-friendly new types rather than aggressive rewrites.

Agent B will create independent JSON fixtures; provide a concise numeric input adapter concept, but do not force B to depend on unfinished C++ API.

## Acceptance tests

Demonstrate without real hardware:
1. Uniform 60 FPS and 120 FPS with accurate elapsed-time rates and frame-gap distributions.
2. Equal mean FPS but bursty gaps; min/max/P95/P99 differ appropriately.
3. Deterministic latency vectors with known avg/min/max/quantiles.
4. Mixed populations or reset/session identities cannot silently yield combined P95/rate.
5. Timeout/missing events are not silently counted as successful latency samples; expose outcome counters and coverage.
6. Expiring windows, empty/startup windows, stale/unavailable data, nonmonotonic time and counter reset.
7. Benchmarked memory bound and capture overhead under >=120 synthetic samples/s and error-burst cases; compare enabled/disabled if possible.
8. CI/CTest target runs without creating Vulkan window, RTSP connection or Jetson device.

The existing UI does not have to adopt the new statistics immediately, but describe the safe migration adapter and any statistical naming/provenance changes in PR.

## Required outcome

Produce small focused PR with public core interfaces, tests and minimal CMake wiring, commit SHA and CI/test evidence. Include examples of snapshot output for (healthy, jittery, no samples, reset). Do NOT claim GPU or Jetson performance validation. Mark all fixture observations synthetic.

## Completion message to coordinator

Summarize API/type names, files and tests, chosen percentile algorithm/error bound, metric ownership/reset/window semantics, performance/memory bounds, unresolved dependencies. Provide branch and PR URL.
