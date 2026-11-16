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
- `GtMapWorker` runs on a dedicated thread with bounded compressed task queue (24 MiB, 32), bounded ready cache (64 MiB, 16 entries, age 5s) and exact FrameKey lookup. It receives **no** `AVFrame` or `AVVkFrame`; it can be skipped under Live pressure without blocking hardware decoder.
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

## Stage 4: verification, release (not yet satisfied)

Required: deterministic parser/worker tests, real producer fixture hashes and negative cases, 1/10/60s domain statistics/contract, Windows CTest, Windows live Vulcan/RTSP/GT smoke, source-vs-Reg video pacing and CPU allocations, Jetson consumer parity. Compare performance versus RGB-only in a common replay, report median/P95/P99 for each observed population. No physical Reg-host or Jetson measurements available here; simulator RTSP/TCP 3174/3174 completeness is **not** Windows Reg FPS. Development preview binaries are for manual bench validation only; do not designate a stable release until hardware smoke and performance acceptance.

## Observability impact

- **RTSP**: packet arrival and existing compressed-byte counts; AU metadata parse/identity errors remain separate from video decode.
- **Decode**: exact Packet-PTS associations (bounded and invalidated on reconnect); optional GT processing never a Vulkan/FFmpeg fallback.
- **Raw**: newest-frame-wins unchanged; existing presented-frame decision counters are not physical scanout; GT layers not yet displayed.
- **GT / worker**: independently count missing, not-ready, invalid and encoded sky/depth; FNV/CRC errors, queue drops/try-lock drops, fully validated maps, pending bytes/frames/age, decompression total/max microseconds. Counters process-lifetime; queue gauges are snapshots; no raw GT or high-cardinality frame labels through HTTP.
- **Blackbox/Replay**: recorder queue losses make Benchmark failure explicit; no guarantee of complete archive after a loss or retention eviction.
- **CVM1, NetImgui, Launcher and Vulkan presentation**: no semantic changes in Stage 1/2.
