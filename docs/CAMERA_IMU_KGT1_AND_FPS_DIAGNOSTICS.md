# KGT1 v1 Camera–IMU receive contract and FPS diagnosis

**Reg implementation:** `src/media/GtSei.hpp`, `src/media/GtSei.cpp`,
`src/video/VideoFrame.hpp`; cross-host producer details remain authoritative at
the simulator. This document describes Reg's accepted shape and validation,
not a claim that a captured simulator payload was available for interoperability testing.

## Wire protocol and bounded parsing

- Existing unregistered-SEI UUID **6242b358-d30d-484a-9ef7-5e3f9c241b07**;
  KGT1 `type="KGT1"`, `version=1`. **RGF1 unchanged**.
- The JSON part, **excluding** the 16-byte UUID, is at most **262144 bytes
  (256 KiB)**, matched to the simulator's declared sender cap. Larger JSON
  is rejected before JSON allocation or traversal.
- KGT1 `camera_imu` is optional. Absent => `Missing`, explicit `null` =>
  `Null`, valid => `Ready`, semantic validation failure => `Invalid`.
  The main KGT1 status remains `Ready` with usable `objects` when
  `camera_imu` alone is semantically invalid. Malformed JSON syntax or
  invalid required KGT1 fields still reject the KGT1 document.
- The parser has depth 24, 8192 allocated value-node maximum, array <= 1024
  entries, object <= 512 members and key length <= 256. GT objects <= 256;
  IMU samples <= **100**. These are parser guards, not simulator sample rates.
- `uint64` fields are parsed directly using integer `from_chars`; no
  intermediate `double` conversions, including for >2^53 values.
- All floating-point sample vectors, calibration and camera world GT parsed
  by Reg must be finite. Sample intervals must satisfy
  `interval_start_ns <= timestamp_ns` and <= 260 ms. Samples must be ordered
  by nondecreasing timestamp, have unique sequences within one KGT1, and
  timestamps must be within roughly 260 ms before and 20 ms after the parent
  simulation timestamp. A 250 ms window at ideal 400 Hz contains up to 100
  measurements. Unit tests cover 100, 101 and malformed numbers.
- Optional embedded `stream_epoch` and `frame_id` in `camera_imu`,
  when present, must match the parent KGT1. For the **parent KGT1**,
  `validateGtForFrame` requires exact (stream_epoch, frame_id,
  source_time_ns) agreement with independent RGF1 and matching visible
  video dimensions; unmatched metadata are **not** displayed or offered
  for same-frame consumer use.
- `camera_imu.simulation_epoch` is not an RGF1 stream epoch.
  A repeated `simulation timestamp_ns` never signifies a duplicate video
  frame. The only video join key is `(stream_epoch, frame_id)`.
- `VideoFrame::cameraImu()` returns the immutable typed structure parsed
  once in `VideoFrame` creation. The GT bbox renderer never reparses the
  history. A CV consumer can use `CameraImuDedup::observe` to receive
  newly-seen samples keyed by **run_id + sequence**. Dedupe is
  bounded (4096 entries by default) and resets on run_id change;
  replay outside the dedupe horizon may be seen again. A caller must
  provide its own single-thread ownership/synchronization.

### Accepted optional typed fields

The extension requires `version`, `run_id`, `simulation_epoch`,
`timestamp_ns`, and `imu` array when non-null. State and status are
optional strings; the camera axes convention, if supplied, must be
`right_down_forward`. K is optional row-major 3×3 in a flat or nested
array; `T_camera_imu` accepts a flat/nested row-major 4×4 or a pose
object with `translation_m[3]` and `quaternion_wxyz[4]`.
Exposure supports `timestamp_ns` and `duration_ns` (instantaneous:
duration 0). `camera_gt` supports world position, quaternion orientation,
velocity, acceleration and gravity, as documented by the typed fields.
Each `imu[]` record requires `timestamp_ns`, `interval_start_ns`,
`sequence`, `sensor_id`, `gyro_rad_s[3]`, `accel_m_s2[3]`,
`status_flags`.

**Interoperability caveat:** No real post-extension payload fixture was
provided from the simulator. The accepted field spellings and layouts above
are Reg's implementation proposal based on the supplied field inventory.
Capture one actual sanitized full KGT1 JSON and assert all expected field
paths in a fixture before declaring end-to-end schema compatibility.

## Observability and interpretation

Reg owns three **distinct** timing boundaries: `PACKET` after demux
(`av_read_frame`; compressed packets, not necessarily one video frame),
`DECODE` after Vulkan video-frame construction and one-time KGT1 parse,
and `PRESENT` on a successful Raw rendering/presentation call (a Reg
presentation decision, **not** evidence of physical scanout). The existing
`logs/frame_timing_*.csv` captures these boundaries and per-frame
`decode_arrival_delta_ms`, `present_interval_ms`,
`decode_to_present_ms`, frame deltas and rendering attempts. The new
columns `gt_json_bytes`, `gt_parse_us`, `camera_imu_state`,
`camera_imu_samples` help separate parser work from transport jitter.
They are emitted by the asynchronous logger, whose queue can drop rows.
A `DECODE` timestamp is after parse; it is not a measure of hardware
decode alone. Exact per-frame receive-to-hardware-decode latency is not
available from these counters without more instrumentation.

In the read-only `/v1/snapshot?sections=video` Diagnostic API
(opt-in on loopback), the following **process-lifetime** counters/gauges
are published with namespaced `gt.*`: KGT1 frames, ready frames,
parse errors, packets over 262144 bytes, IMU ready/invalid/null frames,
IMU samples parsed including window overlap, total successful-KGT1
parsing duration **microseconds**, maximum successful parsing duration
**microseconds**, and maximum observed KGT1 JSON size **bytes**.
The server does not expose raw IMU vectors, positions, identifiers or
arbitrary JSON over the Diagnostic API. Counters are decimal strings;
gauges use finite numeric values. Read-only freshness follows existing
TelemetryModel sampling (250 ms) and DiagnosticRuntime snapshots (1 Hz).
The counters reset on **Reg process restart**, not on stream reconnect;
a decoder session generation and FrameKey must still be tracked for
frame-level analysis.

Video also exports `decoder.demuxed_video_packets_total` as a
process-lifetime counter and
`decoder.demuxed_video_packet_rate_{1s,10s,60s}` as derived rates with
decoder-session boundaries. Existing `decoder.decoded_fps_*`,
`raw.presented_fps_*` and
`overlay.presented_decisions_rate_*` are independent.
Rates are unavailable until the full window is observed, or after a reset.
Disabled Raw/Overlay signals must never be represented as transport failures.
One compressed packet cannot be assumed to equal one decoded frame.
`gt.parse_errors_total` is a metadata compatibility metric, not
evidence of an FPS bottleneck.

**Operator's hypothesis ladder**, with captures on the *same Reg host*:

1. Compare source-side encoded/published frame cadence against Reg's
   `PACKET` arrival delta: source/RTSP/GT waiting or network burstiness
   hypotheses are relevant only with observed gaps here.
2. Compare `PACKET` vs `DECODE` counts, gaps and
   `gt_parse_us`. If packet cadence is smooth but decoding is late,
   measure parse and decoder resource pressure separately.
3. Compare `DECODE` vs `PRESENT`, Raw mailbox frame deltas,
   not-ready attempts, render-call times and surface/reconnect states.
   The Raw newest-frame-wins policy can intentionally skip intermediate
   decoded frames without implying loss at the decoder.
4. Compare with a controlled run without camera_imu (same source,
   resolution, GOP, display, recording, CV and load). Report rates with
   sample windows and confidence, not guessed causes. A test sending
   only malformed GT while video continues isolates parser failures.

### Evidence already provided by the simulator operator

In a **simulator-side** 180-second local TCP receive/decode trial,
**10792 frames** were decoded with no observed omission. The arithmetic
mean is 59.96 frames/s **for that receiver**, not for Windows Reg.
Some delivery gaps reached **96.5 ms**. The simulator may wait up to
**80 ms** for GT; this is an unproven **burstiness hypothesis**, not
a diagnosed cause of Reg FPS drops. Camera pose publication around
25 Hz is not a video frame rate. Reg FPS remains **unmeasured** until
a Reg-host trace is captured and compared.

## Testing

`tests/gt_sei_osd_tests.cpp` includes real-length *synthetic* JSON
(~24 KB) with 100 ideal IMU records, malformed IMU isolation, old KGT1
compatibility, 2^53 precision, RGF1 identity and 256 KiB boundary.
`tests/gt_ingest_stats_tests.cpp` and
`tests/diagnostic_telemetry_adapter_tests.cpp` check counter semantics,
process scope and independent demux/decode/Raw rates.
A passing CI test is not a hardware measurement or confirmation of the
simulator's exact schema. Run Windows CI before publishing the build;
Linux full CI is manual by existing repository workflow policy.
