# Reg Diagnostics API — Product Requirements, Data Contract and Metric Catalog

**Status:** DESIGN / PROPOSED — no API server is implemented by this document.
**Version:** design revision 0.2, target HTTP contract v1.
**Source baseline:** kxsxlxv/Reg, feat/netimgui-russian-detr @ 82641a1760361dbc5d46f441eb03ee1ddd34452a (verified 2026-10-09).
**Owner:** Reg runtime / observability. Companion agent guide: docs/DIAGNOSTICS_AGENT_README.md. Repository agent rules: AGENTS.md.
**Supersedes:** earlier exploratory Reg Diagnostics & Agent Interface PRD, while preserving its read-only, three-host architecture.

## 1. Why it exists

The diagnostic lab has three distinct hosts: Ubuntu runs UNIGINE 2.22 SIM and MediaMTX, Jetson Orin Nano runs the CV producer and CVM1 sender, Windows runs Reg. Today an agent cannot inspect Windows video/Exact Sync state or Jetson inference from Ubuntu without screenshots or manual log transfer.

The API's job is not just monitoring graphs. It must answer:
- Is each component **alive, connected, producing, consuming and fresh**? Which stage stopped first?
- Did an RTSP disruption arise at UNIGINE/publisher, MediaMTX, network, FFmpeg demux, Vulkan decode or presentation?
- Are video frames validly identified? Did CVM1 reach Reg? Did frame keys match? Was missing metadata late, never received, evicted or emitted with zero detections?
- After a controlled restart, when and how did recovery occur, and was the observed result supported by evidence?
- What is the distribution and sample population behind a proposed adaptive CV Overlay delay, including the dropped or unobserved tail?
- What changed between builds, settings, sessions and test runs?

MVP is intentionally **read only**. It must not affect RTSP, GPU decode, rendering, recorder, or CV processing even if clients disappear or send malicious requests.

## 2. Boundary and deployment

### 2.1 Responsibilities

- **Reg process:** owns authoritative RTSP/decode/render, metadata, exact sync, recorder and NetImgui state. Publishes bounded, immutable diagnostic snapshots from the same sources used by the UI.
- **Reg diagnostic transport:** loopback HTTP/JSON listener on a background thread, OR authenticated local IPC plus sidecar if a measured failure-isolation spike favours it. Both must implement the identical v1 contract. No HTTP on render/decode callbacks.
- **Ubuntu collector:** reuses local MediaMTX Control API and metrics; reads an explicitly configured UNIGINE launcher/service and logs, never assumes process ownership.
- **Jetson collector:** instruments CV pipeline at decode, queueing, inference and CVM1 send boundaries; uses local tegrastats/system metrics for hardware. Missing hooks stay unavailable, not fabricated.
- **Lab MCP adapter:** runs near the active Codex agent, usually Ubuntu. It calls the three read-only interfaces and maps them to domain tools; does not own telemetry.
- **Optional deterministic test runner:** separate audited control plane for *approved Ubuntu-local* operations. Not mounted in Reg's read-only API; no remote shell via MCP.

### 2.2 Rollout order

0. Without Reg changes: MediaMTX metrics/API + existing Reg session logs/CSV + a Jetson sampler; prototype correlation and an agent report.
1. Add a stable Reg snapshot API and an HTTP/IPC read adapter with auth/network boundary. Reuse current counters; no new GPU instrumentation initially.
2. Add low-overhead structured events and missing measurements; Jetson and UNIGINE exporters; bounded per-frame trace (off unless requested).
3. Add lab MCP adapter and evidence bundle; optional Prometheus exporter and historical store.
4. Add separate authorized test runner. Much later: Adaptive Delay Observe, hardware trials, optional Auto.

## 3. Fundamental truth and timing contracts

### 3.1 Data classes

Every published metric must state a provenance class:

| Class | Meaning | Example |
| --- | --- | --- |
| measured | Directly instrumented at the named boundary | Reg decoded-frames counter |
| derived | Explicit formula from measured inputs; include time window | FPS computed from two decoded-frame samples |
| external | Measured by another service, with its host/source named | MediaMTX inbound RTP packet loss |
| estimated | Modelled with uncertainty; do not imply a direct measurement | Hypothesized end-to-end latency |
| unavailable | Unsupported, disabled, not instrumented or stale | Jetson inference duration before hooks exist |

A healthy API listener is not evidence that video is healthy. Missing observations are not numerical zero. A CVM1 payload with zero targets is valid metadata, not missing metadata. Sender socket acceptance is not proof of delivery. MediaMTX RTP loss cannot be equated to all lost video frames; network, encoder, demux and decode stages have different loss domains.

### 3.2 Identity

- FrameKey = (stream_epoch, frame_id) extracted from source H.264 SEI. Both unsigned 64-bit values.
- Reg process session, decoder generation, source stream epoch, CV process session and host boot identifiers have different meanings. Never collapse them into one ID.
- A MediaMTX restart need not change stream_epoch if the source encoder continues the same logical stream. A source restart must not reuse an epoch with reset frame IDs.
- test_run_id groups observations from an orchestrated experiment; adding a test marker does not mutate FrameKey.
- All JSON uint64 IDs and counters are **base-10 strings** to avoid precision loss in JavaScript. Rates, bounded millisecond durations and ratios are JSON numbers; negative delay measurements are valid where defined.
- A cursor or host ID is opaque. Clients must not parse its structure.

### 3.3 Time

- observed_at_utc: UTC time at snapshot publication, used for cross-machine approximate alignment.
- sampled_at_utc: time the specific section was last measured. age_ms is measured locally against monotonic time at response generation.
- monotonic_uptime_ms: useful only **within one host boot/process domain**; do not subtract times across machines.
- Clock metadata records ntp_state, offset_estimate_ms and uncertainty_ms if measured; otherwise unknown/null.
- Jetson cvBeginNs/cvEndNs duration is valid on Jetson; Windows receivedAt minus Windows decodedAt is valid on Windows. Jetson monotonic timestamp minus Windows monotonic timestamp is invalid.
- Display latency / scanout is not known merely because Vulkan submitted or presented a frame. It requires separate measurement/instrumentation.

### 3.4 Freshness and counter reset

- A section has state: fresh, stale, disabled, unsupported, uninstrumented, or error; data_age_ms and sampled_at_utc. A missing value uses null plus a structured unavailable reason.
- Counter reset scope is process, decoder_session, stream_epoch, cv_session, source_session or service_boot. A counter can be compared only when its reset scope identity is unchanged; otherwise report reset/unknown delta.
- Never silently carry metrics from the previous stream_epoch as a current CV health reading.
- Status should distinguish service_liveness, transport_connection, recent_signal, and producer_activity.
- Snapshot response sequence is monotonically increasing within API process session; it does not imply capture at every video frame.

## 4. API design

Versioned, localhost-only by default; once implemented proposed default endpoint: http://127.0.0.1:8765 (port configurable). Version 1 is additive; breaking changes require /v2 and an agent migration window.

| Method | Endpoint | MVP | Purpose |
| --- | --- | --- | --- |
| GET | /healthz | Yes | Tiny service liveness only; not stream health |
| GET | /v1/capabilities | Yes | Host role, endpoint support, metric capability states, schema/build |
| GET | /v1/status | Yes | Compact state/freshness, most recent errors, key health indicators |
| GET | /v1/snapshot?sections=rtsp,video,cv,exact_sync,recorder,ui | Yes | Bounded typed section snapshot; no unbounded history |
| GET | /v1/series?metric=...&window_s=60&step_ms=1000 | Phase 1 | Allowlisted numeric histories; bounded and labelled |
| GET | /v1/events?after=CURSOR&limit=100 | Phase 1 | Stable, paginated, redacted event stream |
| GET | /v1/logs?source=reg&after=CURSOR&limit=100 | Phase 2 | Curated/redacted log excerpt; NEVER arbitrary file path |
| GET | /v1/frames/{stream_epoch}/{frame_id} | Phase 2 | Bounded trace for a retained FrameKey; 404 if not retained |
| GET | /metrics | Optional | Low-cardinality Prometheus export, separate from JSON semantics |
| GET | /openapi.json | On first HTTP release | Machine-readable OpenAPI contract generated/tested with source |

Design note: do not implement many overlapping per-component HTTP routes merely for convenience. The snapshot sections and metric catalog should be the primary interface. The MCP adapter can expose user-friendly domain tools over those routes.

### 4.1 Standard envelope

~~~json
{
  "schema_version": "1.0",
  "api_process_id": "opaque-id",
  "host_id": "lab-windows-reg",
  "host_role": "reg",
  "boot_id": "opaque-boot",
  "process_session_id": "opaque-session",
  "test_run_id": null,
  "snapshot_sequence": "142",
  "observed_at_utc": "2026-10-09T19:00:00.000Z",
  "monotonic_uptime_ms": 75123,
  "clock": {
    "sync_state": "unknown",
    "offset_estimate_ms": null,
    "uncertainty_ms": null
  },
  "sections": {
    "rtsp": {
      "freshness": {"state": "fresh", "data_age_ms": 180, "sampled_at_utc": "2026-10-09T18:59:59.820Z"},
      "connection_state": "receiving",
      "decoder_sessions_total": "3",
      "reconnects_total": "2",
      "last_valid_video_frame_age_ms": 24,
      "last_error": null
    },
    "exact_sync": {
      "freshness": {"state": "fresh", "data_age_ms": 180, "sampled_at_utc": "2026-10-09T18:59:59.820Z"},
      "source_frame_key": {"stream_epoch": "4829", "frame_id": "9223372036854775810"},
      "matched_pairs_total": "12045",
      "due_without_metadata_total": "18",
      "metadata_after_dropped_frame_total": "6",
      "arrival_wait_matched_only_ms": {
        "p50": 52.2, "p95": 104.3, "p99": 129.6,
        "sample_count": 256, "sample_population": "matched_pairs_only",
        "window_type": "last_N_matches", "censored_tail": true
      }
    },
    "jetson_inference_duration_ms": {
      "value": null,
      "availability": "external_host_metric",
      "reason": "Use Jetson diagnostics endpoint; never infer from Reg-local clocks"
    }
  }
}
~~~

Illustrative only, NOT live readings or a functioning endpoint. Section freshness may be recent even when an internal stream is disconnected: use section fields to identify actual health. API processing/serialization errors use problem details with a stable error code, request_id and human-readable message, never a stack trace containing secrets.

### 4.2 Request and response rules

- Configurable loopback port and bounded worker count; no public/LAN bind by default.
- Default read timeout 2 s; local collector cache refresh suggested 1 Hz, retained latest immutable snapshot, no direct network operation from rendering threads.
- JSON max 1 MiB response; sections and samples bounded; events/logs default 100, max 1000; series default last 60 s, max 300 s in Reg's in-memory store. These are initial design budgets subject to profiling.
- Reject unknown metrics, unreasonable steps, malformed cursor, invalid frame identities and oversized requests with documented 4xx errors.
- A requested section that is not compiled or disabled must return an explicit availability state, not vanish silently. Unknown optional fields may be ignored by clients.
- Cursor pagination and event ordering stable across a process session; on restart cursor expires explicitly, with a new process_session_id.
- ETag/Last-Modified may be added later; no client should poll per frame.
- Never leak arbitrary disk paths or credentials in errors, logs, snapshots, active profiles or build flags.

## 5. Reg metric and state catalog

Status markings: **EXISTING** means an in-process source exists at the reviewed revision, not that it is remotely exposed. **NEW** requires new instrumentation. **EXTERNAL** belongs to another host. Each field must be registered in a versioned catalog/OpenAPI before an implementation merges.

### 5.1 Process, build, topology and config

| Proposed fields | Source | Semantics |
| --- | --- | --- |
| version, build_sha, binary role, build flags, OS, uptime, process_session_id | NEW | Accurate build fingerprint and process state |
| selected profile label, Raw/CV/Telemetry enabled, configured target displays | EXISTING LauncherConfig/CommandLine | Redacted effective settings; never include RTSP URL credentials |
| RTSP transport, URL host alias/path alias, session decoder generation | PARTIAL existing runtime | Omit secrets; distinguish configured vs negotiated |
| NetImgui enabled, listening, connected clients, received/sent bytes, reconnects | PARTIAL NetImguiHostStatus; NEW historical states | Do not equate remote UI connection to RTSP health |
| launcher child alive, exit code, updater state, last update error | PARTIAL ProcessManager/UpdateManager | Different process: requires separate Launcher adapter; NOT available magically from reg_probe |
| data directory disk free, process CPU/RSS, NVIDIA driver/GPU name/temperature/util | NEW/EXTERNAL | Label host, provider, time and validity; Jetson hardware sourced on Jetson |

### 5.2 RTSP/network/source

| Fields | Source | Diagnostic meaning |
| --- | --- | --- |
| rtsp.connection_state, connected, no_signal, last_frame_age_ms | EXISTING decoder/watchdog | Distinguish TCP/UDP socket/session health from decoded frame freshness |
| rtsp.decoder_sessions_total, reconnects_total, backoff_ms, last_error_code | EXISTING counts; NEW explicit backoff/error codes | Measure retry progress, not merely UI NO SIGNAL |
| rtsp.demux_packets_total, read_stalls_total, demux_errors_total, last_packet_age_ms, bitrate_bps | NEW/partial CSV | Locate packet transport/demux issues without conflating with decode |
| source_frame_epoch, last_source_frame_id, identity_missing_total, identity_nonmonotonic_total | PARTIAL existing runtime | Validate source SEI and epoch transitions |
| MediaMTX publisher_state, reader_count, received RTP packets/loss/errors/jitter | EXTERNAL MediaMTX Control API / metrics | Use its actual metric definition; identify path and session, no synthetic "Reg packet loss" |
| UNIGINE running, encoder FPS, access-unit sequence, publish errors | EXTERNAL Ubuntu instrumentation | Available only if process/probe exposes it |

MediaMTX provides /v3/paths/list, /v3/rtsp/sessions/list and optional /metrics. Never expose the entire MediaMTX Control API to an agent: it includes mutating endpoints. Use only allowlisted GET calls through the local collector.

### 5.3 Video decode / Raw / Vulkan

| Fields | Source | Semantics |
| --- | --- | --- |
| decoder.decoded_frames_total, decoder.output_fps, decoded_format/resolution | EXISTING totals, NEW format/status export | FPS is a delta with declared window; Vulkan-only/format failure is explicit |
| raw.presented_frames_total, raw.present_fps, last_present_age_ms | EXISTING total, NEW freshness | Presentation submitted/display path not same as physical scanout |
| raw.mailbox_overwrite_total and latest-frame age | NEW | Raw intentional newest-frame-wins policy: overwrite is not a FIFO leak |
| decode_to_present_wait_ms, render_attempt_ms, Vulkan not_ready attempts | PARTIAL FrameTimingLogger CSV; NEW aggregate | Wall-clock local measurements; not network end-to-end latency |
| Vulkan device_loss_total, swapchain_rebuild_total, surface recovery, present_mode | NEW | Diagnose GPU and monitor disruptions |
| monitor mapping, dimensions, DPI, window visibility | PARTIAL, needs runtime export | Must be per presentation role; missing monitor distinct from decoder down |
| GPU memory allocated, queue sync stalls, decode frame pool occupancy | NEW only if measured | Avoid inventing driver/GPU counters that are not actually exposed |

### 5.4 CVM1 ingest and Jetson

| Fields | Source | Semantics |
| --- | --- | --- |
| cv.signal_present, last_accepted_packet_age_ms | EXISTING watchdog | Signals valid accepted CVM1 at Reg, not that inference is currently healthy |
| cv.packets_total, invalid_total, duplicates_total, sequence_gaps_total, out_of_order_total | EXISTING TelemetryModel / MetadataReceiver | Verify exact counter increment boundaries before naming accepted vs received |
| cv.store_depth, store_evictions_total, most_recent_frame_key | EXISTING | Bounded store; exact FrameKey required |
| cv.valid_zero_target_snapshots_total | NEW | A healthy CV frame with no objects is not a dropped metadata packet |
| cv.frame_epoch_mismatch_total, cv.metadata_for_unseen_frames_total | NEW | Distinguish stale epoch vs rate/transport mismatch |
| Jetson CV input/decoded/inference FPS, queue depth, age, model/revision, inference p50/p95/p99, sender would-block/drop | EXTERNAL Jetson collector | Sender success means kernel accepted UDP, not Reg delivery; track send/receive separately |
| Jetson GPU utilization, VRAM/RAM, thermal/power and clock-throttle | EXTERNAL tegrastats/system | Provider, units, sampling period, missing-value reason |

### 5.5 Exact frame synchronization and Overlay

| Fields | Source | Semantics |
| --- | --- | --- |
| exact_sync.matched_pairs_total, rejected_key_mismatches_total | EXISTING ExactSyncDiagnostics | Wrong-key pairs must never render |
| exact_sync.due_without_metadata_total, metadata_after_dropped_frame_total | EXISTING | Frame was due but metadata not available; some late metadata may arrive later |
| exact_sync.metadata_arrived_after_deadline_total, matched_after_deadline_total | EXISTING | Separate observed late packet from successful late match |
| exact_sync.last_video_key, last_metadata_key, last_matched_key | EXISTING | Keys require BOTH epoch and frame ID |
| exact_sync.arrival_wait_matched_only_ms.p50/p95/p99, sample_count | EXISTING | Last <=256 successfully matched observations; wait=max(0, Reg receive minus Reg decode). Negative observed waits are clamped to 0 for percentiles. **Selection bias: missing/very-late results are not fully represented** |
| overlay.configured_delay_ms, buffer_depth, evictions_total, missing_identity_total, missing_metadata_drops_total | EXISTING | Deadline configured from decode time, not source display time |
| overlay.playout_due_total, displayed_with_metadata_total, buffering_age_ms and deadline miss reasons | PARTIAL/NEW | Need denominator and reason taxonomy for stable success ratios |
| overlay.delay_mode, recommended_delay_ms, recommendation_confidence, effective_delay_ms, controller_state/reasons | FUTURE Adaptive Delay Observe/Auto | No Auto until Jetson hardware acceptance; controller MUST consume censored/missing cases, not only matched percentiles |

Design warning: OverlayFrameBuffer::setPlayoutDelay() currently recomputes deadlines for buffered frames. Changing delay frequently can cause temporal discontinuities, burst/skip behaviour or memory pressure. Adaptive delay must have its own phase, generation-aware deadline policy and explicit scheduling regression tests before any automatic controller ships.

### 5.6 Blackbox / Replay / events

| Fields | Source | Semantics |
| --- | --- | --- |
| recorder.enabled, packets_written_total, queue_depth, queue_drops_total, metadata_written_total, metadata_queue_drops_total, failures_total | EXISTING Recorder counters | Recorder must never block demux/decode |
| recorder.last_segment_completed_at, free_bytes, retention_actual_s, export errors | NEW | Avoid scanning the full filesystem on requests |
| replay.state, input validity, frame key, seek/speed progress, exact pairs on replay | PARTIAL reg_replay, requires separate process adapter | Replay and live pipeline must be labelled separately |
| events severity/source/type/time/session/epoch, last error and restart reason | PARTIAL TelemetryModel events; NEW event taxonomy | Existing free-text messages are not a stable machine contract |
| bounded sanitized log tail and timing CSV export pointer | EXISTING files via Launcher; NEW authorized reader | No arbitrary file read or uncontrolled file download |

## 6. Structured events

Events are transitions, not per-frame logs. Proposed names:
- process.started, process.exited, process.crashed, diagnostic.overflow
- rtsp.connecting, rtsp.session_opened, rtsp.no_packets_timeout, rtsp.reconnect_scheduled, rtsp.recovered
- video.signal_lost, video.signal_restored, frame_identity.missing, frame_identity.nonmonotonic, frame_identity.epoch_changed
- cv.signal_lost, cv.signal_restored, cv.packet_invalid, cv.sequence_gap, cv.metadata_late, cv.zero_targets
- exact_sync.frame_due_without_metadata, exact_sync.key_mismatch, exact_sync.metadata_after_drop
- renderer.vulkan_device_lost, renderer.surface_recreated, display.topology_changed
- netimgui.client_connected, netimgui.client_disconnected
- recorder.queue_drop, recorder.write_failure, replay.started, replay.completed, config.effective_changed

Each event: event_id, type, severity, observed_at_utc, local monotonic_ms, process_session_id, optional stream_epoch/frame_id, state_before/after and allowlisted diagnostic attributes. Burst-prone per-frame conditions must be aggregated/deduplicated into periodic summaries; never export each frame as a Prometheus label or allocate an event object unboundedly.

## 7. Metric definitions, not merely field names

Every catalog entry and the eventual machine-readable schema MUST capture:
- path, human meaning, kind (counter/gauge/rate/histogram/state/event), JSON type, unit;
- sampling boundary and producer owner, process and reset scope;
- observed freshness and sampling cadence; retained duration/sample count;
- provenance (measured/derived/external/estimated), aggregation formula and denominator/window;
- explicit unavailable/stale representation;
- security classification; stability since API version and supported platform;
- test fixture name or validation scenario.

Recommended format: a catalog definition checked in with OpenAPI and used for example generation and regression tests. Numeric rate names must include per-second semantics; Prometheus uses base seconds for durations and *_total counters. Avoid high-cardinality labels such as frame_id, stream_epoch, session_id, individual target ID or arbitrary URLs.

Required examples of precise definitions:
- decoder_output_fps: delta(decoded_frames_total) / elapsed local seconds across samples with the *same decoder/process reset scope*, never a zero inferred after a restart.
- exact_sync_success_ratio: matched_pairs / eligible_video_frames_with_identity, but only after the denominator and lifecycle accounting are instrumented; do not derive from current snapshot by subtracting arbitrary counters.
- cv_delivery_loss: currently NOT MEASURABLE end to end from UDP sender success and Reg sequence gaps alone; use distinct sender, network and receiver measures and an explicit frame-key coverage study.
- end_to_end_display_latency: currently NOT MEASURABLE from Reg's API without source and scanout instrumentation; label stages only.
- frame_age_ms: local current monotonic time minus last decoded/presented time, null if never seen; do not report 0 when the pipeline has never started.

## 8. Adaptive CV delay data prerequisites

For Observe mode, capture a bounded per-FrameKey diagnostic lifecycle:
decoded_at_reg, metadata_received_at_reg, scheduled_deadline_reg, decision_time_reg,
decision (present/drop/evict), dropped_reason, stream_epoch, frame_id, previous valid metadata presence, source session generation.

Track every eligible video frame, including never-matched and dropped frames, using counters plus an optional bounded ring/sampled trace. Derive delay distributions from appropriately classified matched and late-arriving frames; quantify right-censored never-arrivals and report sample coverage. For arriving-before-decode metadata, record signed local arrival offset and use zero additional wait where applicable. Exclude old-epoch data from a new epoch controller window.

Start with **Manual** + **Observe**. Observe only publishes recommendation, confidence/sample coverage, bounds and reason, never mutates playback. Controller design and deadlines must be validated with Jetson under normal, loss, reorder, burst and restart conditions. See the separate Adaptive CV Overlay Delay PRD.

## 9. Agent-facing MCP tool design

The MCP adapter queries read-only JSON and local collectors. Proposed minimal tools:
- lab.discover(): endpoints, roles, metric capabilities, freshness and clock status;
- lab.get_health(): liveness AND independent video/CV freshness;
- lab.get_reg_snapshot(sections?): bounded Reg status and counters;
- lab.get_exact_sync(window_s?): counters, last FrameKeys, eligible denominator status and sampling bias;
- lab.get_jetson_pipeline(): decode, inference, queue, CVM1 sender, GPU/thermal;
- lab.get_mediamtx(): publisher/reader sessions, RTP errors/loss from native API/metrics;
- lab.get_events(host, after?, limit?): redacted transitions;
- lab.get_log_excerpt(host, source, after?, limit?): curated logs;
- lab.get_frame_trace(epoch, frame_id): optional bounded retained frame state;
- lab.export_evidence(test_run_id, window_s): portable data+manifest, **no videos/screenshots by default**.

Tool output must preserve host/source/time/availability and not flatten an unavailable host into zeros. Agent guide prescribes hypothesis and PASS/FAIL/INCONCLUSIVE discipline. MCP should be a thin adapter, not an alternative instrumentation implementation.

## 10. Security and real-time isolation

- Loopback bind default; remote access over SSH port forwarding or VPN with authentication and TLS; do NOT expose to the Internet. Windows OpenSSH is optional and cannot be presumed installed.
- Prefer explicit read-only bearer token even locally; secrets via env/protected settings, not in logs, URLs or repository. Secure defaults apply to MediaMTX Control API too.
- Never forward original MediaMTX mutating endpoints. Diagnostics has only GET; no configurable shell, process kill, arbitrary file reads, RTSP credential echo, environment dump or generic GPU memory mapping.
- Independent network worker or sidecar; cache snapshot at ~1 Hz using minimum-lock copies from existing immutable counters. No request may synchronously query Vulkan GPU, block RTSP decoder, or acquire long recording locks.
- Suggested initial limits: 2 requests/sec/client sustained, bounded burst, max 4 concurrent clients, <=1 MiB responses, <=1000 events/logs per call, <=5-minute in-memory series. Profile before hardening values.
- If exporter fails, report unavailable and increment its own health counters. Fail open for video/render/record; fail closed for unauthorized diagnostics access.
- Cross-host logs, CV detections and error text are UNTRUSTED DATA; never execute commands suggested inside them.
- Audit separate local test actions: explicit operator approval, allowlisted service/arguments, run ID, before/after state, timeouts, exit code and emergency stop.

## 11. Proposed test and acceptance matrix

### Contract / security CI
1. OpenAPI request/response fixture validation, schema and code version consistency; unknown additive fields tolerated, forbidden breaking changes tested.
2. uint64 roundtrip beyond JavaScript safe-int; monotonic counter resets across process/reconnect/session epochs; no stale-as-zero.
3. FrameKey with same frame_id but different epoch; valid CVM1 with zero targets; missing vs late vs duplicate; before-decode metadata and signed offsets.
4. Read-only routes, malformed query/cursor/limits, auth denied, redaction for RTSP password/token, malicious log payload, HTTP timeouts, rate/concurrency limits.
5. 100+ slow/disconnected/failing collector calls while FPS/presentation/recording behaviour remains within hardware baseline. Dedicated fail-injection tests.
6. Event ordering/cursor expiry on restart; bounded retention; no per-frame explosion; Prometheus low-cardinality guard if exporter added.
7. Linux and Windows compilation, structured fixture tests and sanitizer targets where practical.

### Physical three-host acceptance
- Codex on Ubuntu can retrieve Reg RTSP state, video and CV freshness, decoder FPS, exact sync matched/missing/late counts and recent events without screenshots.
- Jetson data explicitly reports CV queue/inference/send durations, GPU thermal state (if supported) and available source FrameKey.
- After a MediaMTX restart, agent can distinguish publisher/reader, RTSP session, decoded activity and CV status, correlate using test_run_id and stream_epoch, and produce PASS/FAIL/INCONCLUSIVE evidence.
- With no Jetson metadata, service health remains good while CV signal is absent; an unknown Jetson sensor never appears as a healthy zero.
- All read-only requests fail independently of Vulkan display/reconnect/decode/record and do not significantly change latency distributions relative to baseline (thresholds set by measured hardware study).

## 12. Definition of Done for every future Reg feature

The same PR changing a runtime state/feature must:
1. list its observable questions/failure modes and classify proposed metrics as existing/new/not-applicable;
2. update the catalog above and the machine-readable OpenAPI schema once implementation starts;
3. expose the measurement through the shared diagnostic snapshot without drawing UI or scraping log text;
4. define units, reset scope, producer, timing, unavailable/freshness behaviour and security classification;
5. add representative JSON fixtures and metric/event contract tests; add hardware validation where appropriate;
6. update the agent README troubleshooting recipe if interpretation or operational procedure changes;
7. state supported/unavailable platform coverage; document a breaking API change as /v2 rather than silently changing /v1.

A feature is **not diagnostically complete** if users can see a new warning in the GUI but an external agent cannot distinguish its cause using the API. Where a new metric adds unacceptable cost, document the budget and report it as unavailable instead of faking a value.

## 13. Decisions left for implementation spike

- Direct loopback HTTP inside reg_probe vs named pipe + crash-isolated sidecar on Windows.
- Concrete auth secrets lifecycle, Windows SSH/VPN strategy, and API port conflicts.
- Sampling budgets after observing 60/120 FPS traffic and multiple Vulkan swapchains.
- Exact source of process resource and GPU counters; Jetson pipeline hook points.
- Ring retention and per-frame trace cost with enabled Blackbox; handling diagnostic network outages.
- How the orchestrator tags test_run_id across Windows, Jetson and Ubuntu without changing source FrameKey.
- UI/Launcher separate-process telemetry access and whether it warrants a second endpoint.

## 14. Standards and source documents

Project: docs/MVP_ARCHITECTURE.md, docs/FRAME_IDENTITY_SEI.md, docs/RESTART_RECOVERY_VALIDATION.md, docs/METADATA_PROTOCOL.md, docs/JETSON_METADATA_SENDER.md.
Existing code: src/telemetry/TelemetryModel.hpp; src/diagnostics/ExactSyncDiagnostics.hpp; src/diagnostics/FrameTimingLogger.hpp; src/video/OverlayFrameBuffer.cpp; src/metadata/MetadataStore.cpp; src/remote/NetImguiHost.hpp.
MediaMTX: https://mediamtx.org/docs/features/control-api and https://mediamtx.org/docs/features/metrics
Prometheus instrumentation/metric naming: https://prometheus.io/docs/practices/instrumentation/ and https://prometheus.io/docs/practices/naming/
Codex MCP documentation: https://developers.openai.com/learn/docs-mcp
