# Reg deterministic synthetic diagnostics lab (Agent B)

**Offline test fixtures, never hardware measurements.** Python 3.10+ standard library only; no Jetson, RTSP, UNIGINE, MediaMTX, GPU, network, Git integration, or UI needed. This is a test-data producer/oracle, not a Reg exporter, HTTP API, MCP service, or runtime statistics implementation.

## Quick start

Run from the repository root:

```bash
python -m tools.diagnostic_scenarios list
python -m tools.diagnostic_scenarios generate healthy_60 --frames 60 --seed 20261012 --out /tmp/reg-healthy
python -m tools.diagnostic_scenarios verify /tmp/reg-healthy
python -m tools.diagnostic_scenarios summarize /tmp/reg-healthy
python -m unittest discover -s tools/diagnostic_scenarios -p 'test_*.py' -v
```

On Windows, replace `/tmp/reg-healthy` with a local directory. `generate` creates three UTF-8 files: `events.jsonl`, `manifest.json`, `public_manifest.json`. `verify` rejects malformed, contradictory, modified or non-canonical fixtures; returns only a compact count summary and a nonzero exit status on failure. `summarize` provides the same bounded summary. Source fixtures in `tests/fixtures/diagnostics/` can be regenerated with the commands in that directory's README.

## Scenario catalog

| Scenario | Deterministic fault/evidence exercised |
| --- | --- |
| `healthy_60`, `healthy_120` | exact FrameKeys, jitter-free 60/120 FPS Raw and CV baseline |
| `jittered_60`, `irregular_60` | seeded source jitter; explicit 180 ms source gap and catch-up bursts |
| `source_restart`, `mediamtx_restart` | source epoch and ID rollover versus relay restart without epoch change |
| `decode_stall` | transport continues, Reg decoder stalls, reconnect has new Reg session |
| `cv_timing` | CV before Reg decode, late after deadline/drop, missing, duplicate, receive reorder, valid zero targets |
| `cv_zero_vs_missing` | successful zero-target CVM1, no sender packet, local sender success but receiver packet lost |
| `inference_backlog` | CV queue wait and inference slowdown with unaffected Raw |
| `delay_sweep` | 50/100/150/200/300 ms delays, exact matching and finite-capacity eviction |
| `services_independent` | independent NetImgui, recorder and diagnostic freshness failures |
| `clock_reset` | per-host offset/drift and Jetson process/monotonic-clock/counter reset |
| `ambiguous_flood` | unknown/coincidental warnings, 400 repeated events; bounded CLI output |

The generator does not assert that faults would occur on a physical device or that their synthetic timings represent the real Jetson.

## Fixture contract `diag-fixture/v1`

Each JSONL observation contains `schema_version`, `data_origin="synthetic"`, `host_role`, `session_id`, `clock_domain`, `t_monotonic_ns` (unsigned decimal-string nanoseconds), `type`, `frame_key` (null or two unsigned decimal strings `stream_epoch`, `frame_id`), and typed `attributes`. Stream order is deterministic **virtual event time**, not numerical order across unrelated host clocks. Each of four roles has an independently offset/drifting synthetic monotonic clock. Jetson reboot changes both session and clock domain. Never subtract Jetson and Windows monotonic times to estimate latency.

- Source identity is always `(stream_epoch, frame_id)`; both fields are uint64 decimal strings (the default epoch is beyond JavaScript's 2^53 integer limit). Source epoch must change when source IDs restart. MediaMTX or Reg reconnect alone need not change it.
- A JSON `null` `frame_key` means the event is not per-frame; it does **not** mean epoch=0/frame=0. Missing CV is the absence of a received packet and is **not** a valid `target_count=0`. A successful Jetson `cv.sent` is not proof of Windows `cv.received`.
- `overlay.presented_exact` requires a timely `cv.received` for the same exact FrameKey; `overlay.dropped_missing_metadata` and `overlay.evicted` are exclusive terminal outcomes. `cv.late_after_drop` accounts for CV received after the terminal decision. Raw is never delayed by Overlay.
- Owner/session reset identities define counter populations; `cv.sent.attributes.sent_total` resets on Jetson restart. Only compare local timestamps inside a single clock domain and session. The trace's internal virtual scheduler is deliberately **not** included in each public observation.
- Manifest includes scenario/version, seed, nominal FPS, requested source frames, simulated span, delay, per-host clock specs, injected ground truth and deterministic event-count/pacing assertions with unit, sample population and reset scope. Empty pacing populations have null min/max, not numerical zero.
- `ground_truth` and `expected_assertions` are **test-only**, never exported in `public_manifest.json`. Public `fixture_id` is opaque, so the scenario name does not disclose the injected fault to future blind-evaluation consumers. The observation event stream itself has no fault-cause labels.

All three files are canonical (sorted JSON keys, stable newlines) and the manifest pins `events.jsonl` with SHA-256. A verifier also checks observation shape, uint64 boundaries, per-session monotonic clocks, source epoch ID progression, exact-key matches, per-frame terminal accounting, local deadlines and expected counts/pacing. Changing the corpus generator semantics requires bumping `GENERATOR_VERSION` and regenerating fixtures together.

## Limits and integration boundaries

Generation is bounded to 5000 requested frames and 100000 events per invocation; `delay_sweep` intentionally generates five trials. Space/time for generated traces is O(events) (including a sort at O(events log events)); replay checks are O(events) memory/time plus O(source_frames log span) for synthetic clock inversion. Nothing touches Reg's decode or rendering hot paths. The 400-warning scenario stores full small local evidence while displaying only counts, not logs. These bounds are for the **test generator**; they are not limits on or promises about the future production telemetry subsystem.

**Adapter proposal for the coordinator:** consume JSONL in listed order, map `host_role + session_id + clock_domain` into synthetic producer/reset scopes, deliver `type + attributes + nullable full FrameKey` to an offline test harness, let A's owner-local statistics process its own stage populations, and let C's Exact Sync lifecycle model process `video.decoded`, `cv.received`, `overlay.*` decisions. Keep the manifest oracle hidden from the engine until a separate deterministic comparator scores outcomes. No production type/HTTP/schema changes are requested by this PR; event names are provisional, additive fixture vocabulary, and require coordination before runtime instrumentation is wired up.

**Limitations:** intentionally no video bytes, real RTP loss, GPU decode, transport delay measurement, clock synchronization measurement, process control, physical acceptance, or production statistics implementation. Pacing is synthetic ground truth; no matched-only latency percentile is treated as a full CV latency population. The future integration must agree on event names/reset scopes with Agents A and C, including buffer eviction and after-drop attribution.
