# Handoff C — Exact Sync Diagnostic Lifecycle and Honest CV Timing

**Assignment:** correct per-FrameKey lifecycle observability without altering exact matching or rendering decisions.
**Suggested branch:** feat/diag-exact-sync-lifecycle.
**Priority:** P0; start immediately. No Jetson, RTSP server, Vulkan window or GUI required for your tests.

## Why

Current src/diagnostics/ExactSyncDiagnostics.hpp tracks last identities, matchedPairs, dueWithoutMetadata, metadataAfterDroppedFrame, metadataArrivedAfterDeadline and matchedAfterDeadline, and computes P50/P95/P99 of max(0, metadata receivedAt - video decodedAt) only from successfully matched pairs, limited to last 256. Current code stores bounded history for frames dropped at their deadline. It does not measure true end-to-end Jetson inference latency, nor full tail of absent, expired or heavily delayed metadata. src/video/FrameSynchronizer.cpp correctly presents only exact FrameKey and drops missing metadata when due.

Our Adaptive Overlay Delay and agent need honest, differentiated lifecycle accounting and denominators. IMPORTANT: a CV result with zero targets is a valid metadata packet, not a missing result.

## Assignment

Extend diagnostic-only state to distinguish:
- video observed with valid FrameKey; duplicate/nonmonotonic FrameKeys; epoch/session boundary;
- overlay-eligible frame buffered; due and presented with exact metadata;
- due without metadata; buffer evicted; missing identity/ineligible where applicable;
- metadata accepted before decode, between decode and deadline, after deadline but before decision, after video drop, or never observed;
- duplicate, wrong epoch, late packet and valid empty detections;
- bounded pending state and explicit terminal/unknown outcomes without counting unbounded 'never' from absence of evidence.

Include separate metrics with exact denominators/sample populations: eligible_frames_total, presented_exact_total, drop_missing_total, evicted_total, metadata_late_observed_total, metadata_unobserved_at_expiry_total, match_coverage with clear reset scope. Use existing matched-only P50/P95/P99 name or alias with explicit qualifier and sample_count. Quantiles for all results require a documented censoring model; do not substitute successful-only percentile as 'total CV latency'. Local Reg clock durations only; never subtract Jetson cvBeginNs from Windows steady_clock.

Decide what new callback hooks would eventually be necessary from the runtime (e.g. overlay push/eviction, due, rendered, metadata accepted) and document them. For this first PR, prefer adding methods to the diagnostics observer and testing direct calls with fake time, without modifying hot rendering/RTSP code.

## File ownership

OWN:
- src/diagnostics/ExactSyncDiagnostics.hpp;
- tests/exact_sync_diagnostics_tests.cpp;
- if needed, a new adjacent src/diagnostics/ExactSyncLifecycle*.hpp file with pure C++.

MUST NOT MODIFY without approval:
- src/diagnostics/stats/** and tests/diagnostic_statistics_tests.cpp (A);
- tools/diagnostic_scenarios/** and tests/fixtures/diagnostics/** (B);
- src/telemetry/TelemetryModel.hpp or src/main.cpp (coordinator integration);
- src/video/OverlayFrameBuffer.cpp, FrameSynchronizer.cpp, and the active CV render policy. This stage is observability only.
- root CMakeLists.txt unless unavoidable; the existing exact_sync_diagnostics_tests.cpp is already a registered test.

New public events/metrics must have unit, provenance, reset/session semantics and documented meaning. Never call an unobserved response 'UDP loss' solely because metadata did not arrive.

## Test matrix

1. Healthy ordered frames exact metadata at 50 ms, 100 ms delay -> valid presentation.
2. Metadata arrives before frame decode; local signed delta is negative while additional wait is zero.
3. Metadata after deadline but before present decision; after frame drop; never arrives within bounded retention.
4. Same frame_id across different stream_epoch, no false match.
5. CVM1 valid zero targets, duplicates/reordered completions and unmatched metadata.
6. Buffer eviction while waiting, repeated frame keys, processing across session/epoch reset.
7. Admission into one lifecycle stage only once; no double counting upon duplicate callbacks or metadata arrivals.
8. 256+ frame bounded diagnostic history, expiry and unknown outcomes; no growth on extended absence.
9. Matched-only quantiles stay conditional; missing/late counts expose selection bias.
10. Deterministic event and counter snapshots reproducible at synthetic virtual times; correct concurrency locking without expensive hot-path sorts.

## Explicit non-goals

Do not implement adaptive playout control, reassign delays to queued frames, infer end-to-end GPU/Jetson delay from cross-host clocks, change synchronization policy to nearest timestamp, create GUI/MCP/HTTP server or make claims about live hardware.

## Required result

Separate PR with pure diagnostic code, regression tests and a documented integration-hook list for the coordinator. Do not alter existing display behaviour. Ensure old Exact Sync tests continue passing and no change to FrameSynchronizer's exact matching policy.

## Completion message to coordinator

Report lifecycle state model, units, reset/censoring semantics, memory bounds, test matrix outcomes, proposed runtime hooks and needed telemetry contract changes. Include PR URL, branch/commit, and note any device-only assumptions still unvalidated.
