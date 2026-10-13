# Diagnostics offline integration acceptance

**Status:** test-only integration, no RTSP, UNIGINE, Jetson, Vulkan device or GPU.

This PR adds a local deterministic acceptance route connecting:
- **B**: `tools/diagnostic_scenarios/lab.py` generates strictly synthetic multi-host JSONL evidence with a hidden, trusted test oracle;
- **A**: the compiled `reg::diagnostics::stats::DurationWindow` / `FramePacingWindow` accumulate real C++ histogram statistics and frame-gap observations;
- **C**: the compiled `reg::diagnostics::ExactSyncDiagnostics` consumes Reg-side lifecycle observations, preserving full epoch+frame IDs and distinguishing exact-presented/missing/evicted/late metadata and valid zero-target packets.

## How the bridge works

The Python scorer generates a short scenario with a fixed seed and projects **only synthetic Windows Reg-local** events to a bounded 6-field whitespace row protocol on a subprocess stdin pipe. The bridge is *not* an HTTP/MCP API or production event parser; it performs no network calls and reads no live logs. It receives no injected fault labels, no ground-truth oracle and **no Jetson or source timestamps** that could be incorrectly subtracted from Reg-local time.

It processes `video.decoded` / `cv.received` / `cv.duplicate` / `overlay.presented_exact` / `overlay.dropped_missing_metadata` / `overlay.evicted`; uses the original source FrameKey to join them in real C++ code; calculates matched-only local wait and independent missing/evicted outcomes using the actual histogram; and returns one short status line. The Python scorer independently computes expected frame counts, terminal outcomes, valid-empty detections and exact matched-only wait P95, then checks the histogram conservatively upper-bounds the exact P95 within its documented 6.25% bin error.

Currently the runner exercises 13 scenario families, including healthy 60/120 FPS, jitter, source restart, MediaMTX restart, mixed CV timing/zero-vs-missing, Jetson backlog, delay sweep and warning floods. The `decode_stall` fixture deliberately changes Reg process session, and is **not** yet projected: a production adapter must instantiate separate per-session statistic series and reset the Exact Sync observer at the correct process/decoder boundary. This omission is explicit, not an assertion of session-safe integration.

**Important:** bridge-side `overlay.presented_exact` is an Exact Sync *selection decision*, not proof that Vulkan completed presentation. Synthetic `cv.sent` does not prove receipt. No cross-host causal or real-time claims are made. The test-only delay mapping for Overlay eviction events is derived from the scenario generator, not from a real runtime callback. Live integration will require the actual evicted FrameKey and deadline.

## Run offline with full project build

    cmake -S . -B build -DREG_BUILD_TESTS=ON
    cmake --build build
    ctest --test-dir build --output-on-failure -R 'diagnostic_scenarios_python|diagnostic_fixture_bridge|diagnostic_statistics|exact_sync_diagnostics'

The full project itself may require Vulkan/FFmpeg setup for configure; **the bridge process does not use graphics or media**. For a strictly dependency-free native build, use `src/diagnostics/stats` standalone for the statistics tests and the Python generator's standalone tests. The unified CTest on full Reg Windows/Linux CI runs both the 21 Python unit tests and cross-module bridge tests.

The bridge is not a substitute for future full three-host HIL acceptance. Changes to runtime callbacks, provenance, frame processing, API or statistics populations must be verified separately.

## Dependency and observability boundary

This test does not modify `TelemetryModel`, RTSP decode, `FrameSynchronizer`, `OverlayFrameBuffer`, recording or Launcher. Nothing runs in a real-time loop. It establishes an offline reference suite for later integration of producer-owned statistics and Exact Sync lifecycle hooks. The C++ API/histogram computations, rather than generated Python/LLM calculations, are the system under test. Historical source-local pacing is covered by B's fixture oracle; the bridge uses only Reg decoder pacing.
