# Simulator GT ingest integration — stages and acceptance

**Status:** development / draft PR #53; not a production release. Based on `ba147d67`. Source of truth is the user-supplied 2026-11-16 simulator handoff package (`PROTOCOL.md`, actual binary fixtures, reference decoders). The implementation does **not** change the simulator protocol or CVM1.

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

**Known gaps:** full independent golden byte-for-byte C++ fixtures are not yet checked into GitHub; the archived binary fixtures were verified separately using the supplied Python reference decoder. The existing rolling recorder archives compressed packet AUs but does not yet create a standalone, complete *non-rolling* benchmark manifest or durable synchronized GT-by-PTS journal with explicit quota checks. Live and Benchmark share the same recorder settings and Benchmark can report failure; Benchmark retention guarantees are not yet complete.

## Stage 3: Raw GT inspection UI (not yet implemented)

- Current Raw bbox+ID OSD and GT telemetry panel remain unchanged.
- **Not implemented yet:** GPU scalar texture upload and multi-view sky/depth comparison, independent display toggles for every GT layer, world pose/IMU time-series UI, cursor readout in metric depth, and freeze-locked RGB+GT inspection. These must not be advertised in a release.

## Stage 4: verification, release (not yet satisfied)

Required: deterministic parser/worker tests, real producer fixture hashes and negative cases, 1/10/60s domain statistics/contract, Windows CTest, Windows live Vulcan/RTSP/GT smoke, source-vs-Reg video pacing and CPU allocations, Jetson consumer parity. Compare performance versus RGB-only in a common replay, report median/P95/P99 for each observed population. No physical Reg-host or Jetson measurements available here; simulator RTSP/TCP 3174/3174 completeness is **not** Windows Reg FPS. **Do not publish a development release of this integration until acceptance.**

## Observability impact

- **RTSP**: packet arrival and existing compressed-byte counts; AU metadata parse/identity errors remain separate from video decode.
- **Decode**: exact Packet-PTS associations (bounded and invalidated on reconnect); optional GT processing never a Vulkan/FFmpeg fallback.
- **Raw**: newest-frame-wins unchanged; existing presented-frame decision counters are not physical scanout; GT layers not yet displayed.
- **GT / worker**: independently count missing, not-ready, invalid and encoded sky/depth; FNV/CRC errors, queue drops/try-lock drops, fully validated maps, pending bytes/frames/age, decompression total/max microseconds. Counters process-lifetime; queue gauges are snapshots; no raw GT or high-cardinality frame labels through HTTP.
- **Blackbox/Replay**: recorder queue losses make Benchmark failure explicit; no guarantee of complete archive after a loss or retention eviction.
- **CVM1, NetImgui, Launcher and Vulkan presentation**: no semantic changes in Stage 1/2.
