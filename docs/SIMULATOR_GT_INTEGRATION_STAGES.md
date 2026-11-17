# Simulator GT ingest integration — stages and acceptance

**Status:** experimental sky/depth GPU display feature branch on top of draft PR #53; not a production release. Based on `ba147d67`. Source of truth is the user-supplied 2026-11-16 simulator handoff package (`PROTOCOL.md`, actual binary fixtures, reference decoders). The implementation does **not** change the simulator protocol or CVM1.

## Stage 1: compressed-AU and packet-PTS association (implemented, CI pending)

- `H264AuSei.hpp` scans complete H264 compressed access units as Annex-B or declared avcC 1/2/4-byte prefixes, removing emulation prevention and processing SEI type 5 with extended sizes up to 8 MiB. Four recognized UUIDs: RGF1, KGT1, KSM1 and KDM1. Unknown messages are ignored.
- Each recognized payload is separately owned; identical duplicates count once and conflicting duplicates invalidate only the affected field. Malformed/truncated AU is explicit, never attached to a different decoded image.
- `GtPtsAssociation` records in a bounded 2 s / 32 MiB / 128-entry pool keyed by packet presentation timestamp, not packet arrival or DTS. Decoded `AVFrame` association uses PTS and is correct under a simulated reorder. Unknown/duplicate/colliding PTS must not be joined speculatively. A codec-session/source-epoch change clears pending associations.
- `VideoFrame` owns immutable `FrameGtParts` with independent statuses and a single parsed KGT1; its Vulkan frame/surface is not copied to CPU. Fallback AVFrame SEI extraction is used only when a compressed-AU association could not be made; hardware paths are not presumed to preserve every unregistered SEI.
- KGT1 bbox status no longer suppresses ready Camera/IMU. Typed fields include actual `rotation_optical_to_world_wxyz`, projection, K and optical frame conventions; integer sensor IDs remain exact decimal tokens.
- The decoded image and independent GT parts must agree on `stream_epoch, frame_id, source_time_ns` before display or scoring; simulation time and sender-local ready time are never substituted for the FrameKey.

**Known gap:** the real short clip does not contain B-frames, so hardware decoded-PTS behavior under reorder remains a hardware acceptance condition. Robust live packet-framing handling also requires a real Windows RTSP test.

## Stage 2: off-path map decode and capture policy (partly implemented)

- `GtMapDecode` validates KSM1 bitpack/RLE8, LSB0 validity and FNV-1a, and KDM1 raw/Zstd shuffled float32 metres with CRC32, finite/positive pixels and exact expected lengths. It preserves canonical scalar bytes without normalization, colorization or CPU video readback.
- `GtMapWorker` runs on a dedicated thread with bounded compressed task queue (24 MiB, 32), bounded fallback ready cache (96 MiB, 128 entries, age 5s) and per-AU immutable decoded results linked to each video frame and exact FrameKey lookup. It receives **no** `AVFrame` or `AVVkFrame`; it can be skipped under Live pressure without blocking hardware decoder.
- CLI `--gt-capture-policy live|benchmark` defaults to Live. Benchmark requires recorder enabled. The existing asynchronous compressed recorder sees all demuxed original AUs before latest-frame display; queue overflow/failure makes the benchmark capture explicitly failed, not silently lossless.
- Per-field statuses and worker queue/CRC/decode counters are exported in the existing opt-in read-only diagnostic API video section. Quantiles and fresh hardware measurements are not inferred.

- Benchmark now writes an additional **non-rolling RGAU1 archive** via `GtAuJournal`, storing the original ref-counted H264 packet bytes (before Raw display), decoder-session generation, bit-exact signed PTS/DTS and timebase, and per-record CRC32. It uses an asynchronous 64 MiB / 1024 packet queue, explicit 8 GiB archive quota and a completion manifest with decimal-string counters. Quota, queue and file errors are sticky failure conditions; the companion reader validates CRC/bounds/truncation. No AU re-encoding or re-muxing is performed in RGAU1.

**Known gaps:** full independent golden byte-for-byte C++ fixtures are not yet checked into GitHub; the archived binary fixtures were verified separately using the supplied Python reference decoder. The RGAU1 archive preserves each compressed AU and packet timing, allowing FrameKey recovery from the original SEIs; an independently verified source-manifest completeness oracle, crash-durable journal finalization and record-level permanent GT-by-PTS query index are still outstanding. A clean per-process archive manifest is only a local record of accepted/written packets, not a proof of producer completeness.

## Stage 3: Raw GT inspection UI (sky/depth overlays implemented in feature branch)

- The GT telemetry page now exposes **independent** Raw display toggles for BBox, KSM1 sky and KDM1 depth, with sky/depth alpha sliders. Depth uses a fixed editable **optical Z metre** interval (near/far) for shader colour mapping, distinct from root \`range_m\`. Near = warm red/orange, mid = yellow, far = blue. Invalid depth (Z=0) is transparent; no per-frame min/max auto-normalization that would make colours incomparable.
- Vulkan fragment shader \`shaders/gt_map.frag\` composites both layers between the existing video pass and ImGui BBox, using two GPU scalar images per in-flight slot. It samples sky's original checksum-verified LSB0 bit-packed mask including optional validity plane, and KDM1 canonical little-endian float32 \`R32_SFLOAT\` optical Z. The compressed map worker owns no AVFrame or VkImage; there is **no decoded-video CPU readback** and no one-rectangle-per-pixel rendering.
- \`GtDisplaySelection\` enforces exact \`stream_epoch, frame_id, source_time_ns\` and source visible raster for **each layer independently**. The compositor uploads only worker-validated maps; missing/late/not-ready masks do **not** show a cached texture from an adjacent frame. OSD remains disabled by default and regular RGB rendering is untouched until a layer is explicitly toggled.
- GPU resource reuse is fence-scoped for two presentation frame slots; dynamic swapchain color formats rebuild the dedicated blend pipeline; reconnect destroys old GT textures on the render thread. Vulkan errors in optional map setup log a warning and leave RGB + BBox rendering active.

**Still outstanding:** side-by-side depth/RGB/sky comparison, cursor optical-Z readout, freeze-locked RGB+GT inspection, world pose/IMU time-series UI, actual GPU validation/performance/FPS on the Windows test bench. Shader compilation and Windows CI are not a substitute for an actual Vulkan RTSP smoke test.

### Native-size KSM1/KDM1 → Raw RGB upscaling (November 17 follow-up)

- Verified simulator fixture: RGB output **1920×1080**; both KSM1/KDM1 maps **480×270**; `source_size=[2112,1188]` is the renderer's internal sampling raster, **not** RGB frame dimensions. Never reject maps because native `source_size` differs from RGB. Identity remains strict on `stream_epoch`, `frame_id`, `source_time_ns`; each map must fit its own declared native raster.
- Both layers automatically fill the **visible RGB video viewport** (letterbox-safe) using normalized UVs, regardless of map input resolution. This is GPU resampling; Reg does not ask the simulator to generate 1920×1080 KSM1/KDM1, and canonical GT bytes are never resized.
- Separate UI toggles select Nearest or validity-aware Linear upscaling for each map. Default sky Nearest preserves hard binary boundaries. Default depth Linear averages only positive valid Z readings: Z=0 cannot dilute valid depth. Linear sky interpolates post-unpack binary coverage/validity as alpha, never compressed bytes.
- A dedicated pre-FFmpeg decode callback enqueues compressed AU map metadata into the existing non-blocking GT worker before video decode, allowing CPU zstd/CRC verification to overlap Vulkan decoding. The worker's per-map counters, queue age and drops are visible on the GT tab. CRC32 is now lookup-table based instead of bit-at-a-time for 60 FPS.
- MediaMTX `reader is too slow` means MediaMTX's output path cannot drain quickly enough; it is a separate sender/network/burst/reader issue, **not evidence of GT shader failure**. Check per-AU payload bitrate and packet loss as well as the Reg receive/decode counters. The current MediaMTX queue setting is normally `writeQueueSize`; increasing it only absorbs bursts and cannot solve sustained bandwidth saturation.

### GT telemetry dashboard: compact cards & log optical-Z scale (November 17)

- Match the established Launcher pattern (\`src/launcher/main.cpp\`): three equal-width auto-height coloured cards, labelled **GT BBOX**, **GT Небо**, **GT Карта глубины**, in a single row. Retain independent enable, BBox ID/range, sky/depth alpha, and Nearest/Linear controls. Depth has two compact numeric Z bounds.
- Default far optical-Z is increased from **250 m to 5000 m** for aerial scenes: the previous 250 m limit saturated all farther pixels to blue even with a logarithmic transfer; users can still edit the fixed near/far bounds.\n- Replace static verbose help text with hover tooltips. KSM1 has a cyan alpha-over-checker preview; KDM1 has a GPU-palette-matched three-stop colour ramp with **near / geometric midpoint / far in metres**. The ramp and shader use the same fixed logarithmic normalisation: \`t = clamp(log(max(z,near)/near) / log(far/near), 0, 1)\`. Invalid Z=0 remains fully transparent. No per-frame auto-scaling.
- Hide all packet, parsing, IMU, queue, cache, per-AU and delay counters behind the initially **collapsed DEBUG** section. The expanded section lives in a separately scrolling child, leaving the three cards usable even when DEBUG is long. Collection and diagnostic APIs are unchanged.
- A new pure-math \`GtDepthScale.hpp\` helper and \`gt_map_decode_tests\` check log near/mid/far, in particular that log midpoint is geometric, not arithmetic. GLSL is separately compiled to SPIR-V by the Windows build.
- This redesign changes only the display interface and scalar-map colour transfer, **not** the KDM1 depth values, producer wire format, packet pacing, image sampling, or the FrameKey/PTS association.

## Stage 4: verification, release (not yet satisfied)

Required: deterministic parser/worker tests, real producer fixture hashes and negative cases, 1/10/60s domain statistics/contract, Windows CTest, Windows live Vulcan/RTSP/GT smoke, source-vs-Reg video pacing and CPU allocations, Jetson consumer parity. Compare performance versus RGB-only in a common replay, report median/P95/P99 for each observed population. No physical Reg-host or Jetson measurements available here; simulator RTSP/TCP 3174/3174 completeness is **not** Windows Reg FPS. Development preview binaries are for manual bench validation only; do not designate a stable release until hardware smoke and performance acceptance.

## Observability impact

- **RTSP**: packet arrival and existing compressed-byte counts; AU metadata parse/identity errors remain separate from video decode.
- **Decode**: exact Packet-PTS associations (bounded and invalidated on reconnect); optional GT processing never a Vulkan/FFmpeg fallback.
- **Raw**: newest-frame-wins unchanged; existing presented-frame decision counters are not physical scanout; GT sky/depth overlays are displayed as independent optional Vulkan layers on matching frames.
- **GT / worker**: independently count missing, not-ready, invalid and encoded sky/depth; FNV/CRC errors, queue drops/try-lock drops, fully validated maps, pending bytes/frames/age, decompression total/max microseconds. Counters process-lifetime; queue gauges are snapshots; no raw GT or high-cardinality frame labels through HTTP.
- **Blackbox/Replay**: recorder queue losses make Benchmark failure explicit; no guarantee of complete archive after a loss or retention eviction.
- **CVM1, NetImgui, Launcher and Vulkan presentation**: no semantic changes in Stage 1/2.
