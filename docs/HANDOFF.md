# Reg MVP — Engineering Handoff and Next Implementation Plan

**Handoff date:** 2026-09-28  
**Repository:** `kxsxlxv/Reg`  
**Canonical continuation branch:** `mvp/h264-frame-id-sei`  
**Canonical top PR:** #23 — Phase C3: canonical H.264 FrameIdentity SEI NAL builder

> **Important:** do not continue implementation from `master`. The MVP is currently maintained as a stacked draft-PR chain. New work should branch from `mvp/h264-frame-id-sei` unless the stack has been merged/rebased first.

---

## 1. Product goal

Reg is a cross-platform NVIDIA-only low-latency H.264 RTSP viewer/recorder for Ubuntu 26.04 and Windows 10/11.

The source is a UNIGINE 2.22 SIM render machine running Ubuntu. It renders the scene, H.264-encodes it, and exposes the stream through RTSP/RTP over UDP.

The same source stream is consumed by:

1. the Reg display application;
2. a headless Jetson Orin Nano 8 GB running JetPack 7.2 / CUDA 13.2.

Jetson runs CV inference and sends per-frame metadata back to Reg over UDP.

The operator presentation is conceptually:

- **Monitor 1 — Raw:** newest available decoded video frame, minimum added latency;
- **Monitor 2 — Overlay:** delayed video frame plus CV overlay, exact frame-accurate synchronization;
- **Monitor 3 — Telemetry:** logs, event timeline, targets, FPS/latency/network/recorder telemetry.

The current target stream is 1920x1080 @ 60 FPS, H.264, 8-bit 4:2:0 NV12, no B-frames, ultra-low-latency configuration. Resolution is not hard-coded, but it is assumed not to change inside one active stream session.

Future performance validation should include 1080p120 and possibly 1440p.

---

## 2. Non-negotiable architecture invariants

These constraints are more important than convenience. Do not violate them while extending the implementation.

### 2.1 No OpenGL

OpenGL is forbidden.

This includes:

- no application OpenGL calls;
- no OpenGL rendering backend;
- no toolkit/backend that can silently fall back to OpenGL;
- no OpenGL-based ImGui backend.

The project intentionally builds Dear ImGui core with `imgui_impl_vulkan.cpp` only.

SDL is used only to create/manage Vulkan-capable native windows and process platform/display events.

### 2.2 No decoded-frame CPU roundtrip

After hardware decode, a video frame must not go:

```text
GPU -> CPU pixels -> GPU
```

for normal display, overlay, recording, or replay.

The live path is:

```text
RTSP/RTP/UDP
  -> FFmpeg H.264 hardware decode
  -> AV_PIX_FMT_VULKAN / AVVkFrame
  -> Vulkan YCbCr sampling
  -> Vulkan swapchain
```

Explicit screenshots are the only intentional GPU -> CPU pixel readback path, and only for the requested frame.

Recording uses original compressed H.264 packets, not decoded pixels.

### 2.3 One decode path

Raw and Overlay share the same hardware-decoded frame pool.

There must not be a second decoder solely for the delayed overlay path.

### 2.4 Raw is newest-frame-wins

Raw is not a FIFO.

If rendering/display falls behind, obsolete Raw frames are skipped. The architecture must not accumulate latency in order to preserve every frame.

### 2.5 Overlay is exact-frame-only

Overlay correspondence is defined only by:

```text
FrameKey = (stream_epoch, frame_id)
```

Do not match by:

- wall-clock time;
- nearest timestamp;
- RTP timestamp proximity;
- UDP arrival order;
- "latest metadata";
- previous frame metadata;
- a fixed frame offset.

If exact metadata is missing at the buffered frame deadline, that overlay frame is dropped.

### 2.6 Overlay may intentionally lag Raw

CV latency is variable, roughly from single-digit milliseconds to approximately 100 ms based on current expectations.

Raw remains minimum latency.

Overlay intentionally keeps a bounded GPU-backed history and displays an older frame only when the exact metadata for that frame is available.

Current default overlay playout delay is 150 ms. This is a starting value, not yet a measured production optimum.

### 2.7 Disk and metadata work must not stall the realtime video path

The blackbox recorder receives cloned compressed packets through a bounded non-blocking queue.

Disk/muxer I/O is performed on a dedicated recorder worker.

Metadata receive is independent of decode/render.

Recorder queue overflow or recorder disk failure degrades recording; it must not stop Raw video.

---

## 3. Current architecture

### 3.1 Core technology choices

- Language: C++23
- Build: CMake
- Window/platform layer: SDL3
- GPU API: Vulkan 1.3+
- Decode/demux: FFmpeg
- Overlay/telemetry vector/text rendering: Dear ImGui core + Vulkan backend only
- GPU vendor: NVIDIA only
- Metadata transport: custom binary UDP protocol
- Blackbox video format: Matroska/MKV remux of original H.264
- Blackbox metadata format: project `.cvmj` journal
- Replay: same FFmpeg Vulkan decode/render path as live

### 3.2 Vulkan/FFmpeg ownership

The application creates the Vulkan instance/device itself and binds FFmpeg's Vulkan hardware context to that application-owned Vulkan device.

Decoded frames remain FFmpeg-owned Vulkan images.

`VideoRenderer` samples the NV12-compatible Vulkan image directly using Vulkan YCbCr conversion.

Renderer-side cached `VkImageView` objects retain a reference to the corresponding FFmpeg `AVHWFramesContext`. This is required so a reconnect/recreated decoder pool cannot be freed while the renderer still owns views into its `VkImage` objects.

### 3.3 Multi-window model

Raw, Overlay and Telemetry are separate Vulkan windows/surfaces/swapchains sharing one Vulkan device.

Presentation policies are intentionally different:

- Raw prefers IMMEDIATE, then MAILBOX, then FIFO;
- Overlay prefers a stable MAILBOX/FIFO path;
- Telemetry is independently paced and must never become Raw's pacing source.

Telemetry is rendered at approximately 10 Hz.

### 3.4 Display recovery

The canonical display-recovery implementation is PR #18.

It includes:

- SDL connected-display enumeration;
- initial role placement:
  - Raw -> display ordinal 0;
  - Overlay -> display ordinal 1;
  - Telemetry -> display ordinal 2;
- graceful fallback when a requested display is unavailable;
- display topology/hotplug detection;
- typed `SurfaceLostError`;
- per-window recovery for `VK_ERROR_SURFACE_LOST_KHR`;
- independent Raw/Overlay/Telemetry WSI retry loops;
- surface/swapchain recreation without restarting decoder/recorder.

`VK_ERROR_DEVICE_LOST` is intentionally handled at a different level.

### 3.5 Device-loss recovery

PR #21 adds a device-level watchdog.

`VK_ERROR_DEVICE_LOST` triggers a complete in-process GPU runtime rebuild:

- stop RTSP/decode thread;
- stop metadata receiver;
- stop recorder worker;
- release FFmpeg Vulkan HW contexts;
- destroy Raw/Overlay/Telemetry renderers;
- destroy swapchains/surfaces;
- destroy Vulkan device/instance;
- recreate SDL/Vulkan/FFmpeg/rendering stack;
- reconnect and continue in the same process.

Persistent device failures use bounded recovery backoff.

### 3.6 RTSP reconnect recovery

PR #16 adds automatic RTSP session reconnect.

Important detail: reconnect is not allowed to race renderer lifetime.

When an active decoder session fails, the decoder thread requests render-thread cleanup. The render thread:

1. clears stale Raw/Overlay/metadata state;
2. releases pending synchronized frames;
3. clears target history;
4. waits/retire GPU work;
5. resets video renderer session resources;
6. acknowledges cleanup.

Only then may the decoder thread open the next RTSP session/frame pool.

---

## 4. Frame identity

Exact synchronization depends on a source-assigned identity carried inside H.264.

### 4.1 Carrier

H.264:

```text
SEI user_data_unregistered
```

Project UUID:

```text
7f 53 3b 8d 1a 91 4c 2d 9f 6a 52 45 47 46 49 44
```

### 4.2 Canonical payload v1

32 bytes, little-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic = `RGF1` |
| 4 | 2 | version = 1 |
| 6 | 2 | payload size = 32 |
| 8 | 8 | `stream_epoch` |
| 16 | 8 | `frame_id` |
| 24 | 8 | `source_time_ns` |

`source_time_ns` is diagnostic/playback timing. It is not a correspondence key.

### 4.3 SEI NAL builder

PR #23 adds `buildFrameIdentitySeiNal()`.

It produces:

- H.264 NAL type 6;
- SEI payload type 5 (`user_data_unregistered`);
- UUID + canonical payload;
- correct payload-size encoding;
- `rbsp_trailing_bits`;
- H.264 emulation-prevention insertion;
- Annex-B framing;
- 4-byte-length AVCC framing.

This closes the portable byte-level construction problem.

**Still missing:** the actual UNIGINE/encoder-specific hook that inserts the generated SEI NAL into the same access unit as the corresponding rendered source frame.

See `docs/FRAME_IDENTITY_SEI.md`.

---

## 5. Jetson -> Viewer metadata protocol

See `docs/METADATA_PROTOCOL.md`.

Current protocol properties:

- UDP over LAN;
- one FrameMetadata message per datagram;
- little-endian explicit serialization;
- magic `CVM1`;
- CRC-32C;
- wrapping uint32 packet sequence for transport telemetry only;
- exact source `stream_epoch` + `frame_id`;
- diagnostic CV begin/end timestamps;
- maximum 32 targets;
- target fields:
  - stable target ID;
  - class ID;
  - flags;
  - confidence;
  - normalized bbox.

Normalized geometry coordinates are source-video coordinates, not monitor pixels.

The Viewer owns video-to-screen transform, letterboxing/crop/zoom/pan.

Dense masks are intentionally not part of CVM1 and need a separate extension/transport.

---

## 6. Overlay renderer

The renderer-independent overlay model currently supports semantic primitives including:

- solid/dashed/dash-dot line styles;
- line;
- polyline/history trail;
- rectangle/bbox;
- filled rectangle;
- circle;
- crosshair;
- UTF-8 text with background;
- per-element alpha-compatible color;
- global/master alpha.

The live target scene currently builds:

- bbox fill;
- bbox outline;
- crosshair;
- recent trajectory;
- ID/class/confidence label.

Text is rendered using Dear ImGui's Vulkan path and a system font; Cyrillic-capable fonts are preferred. Text is not CPU-rasterized from scratch per label per frame in the video pipeline.

The transform is centralized in `VideoTransform`, so overlays follow video aspect fit and future zoom/pan consistently.

### Missing overlay/data work

The generic rendering model is broader than CVM1. The metadata protocol currently carries only bbox-oriented target records.

If the product needs Jetson-provided:

- arbitrary polygons;
- segmentation masks;
- arbitrary lines;
- circle primitives;
- explicit trajectory geometry;

define protocol extensions instead of overloading the base target record.

For dense masks, use a separate bounded transport/payload rather than forcing them into the <=1400 byte CVM1 datagram.

---

## 7. Blackbox recorder

### 7.1 Video

The recorder consumes the original compressed H.264 `AVPacket` stream before decode.

It does not re-encode decoded video.

Properties:

- bounded non-blocking packet queue;
- dedicated recorder thread;
- Matroska/MKV output;
- segments begin on a keyframe;
- rotation occurs on a keyframe after target duration;
- default segment target: 5 s;
- default retention: 5 min;
- disk failure is isolated from live decode/render.

### 7.2 Metadata

Validated non-duplicate CV metadata is written to rolling `.cvmj` files.

The journal preserves:

- `stream_epoch`;
- original validated CVM1 payload;
- relative receive timing.

Replay correspondence still uses exact `FrameKey`, never the receive timestamp.

### 7.3 Replay

The canonical replay implementation is PR #19, `reg_replay`.

It:

- opens recorded MKV through FFmpeg;
- still requires Vulkan H.264 decode;
- does not use software decode fallback;
- loads rotated `.cvmj` records;
- indexes metadata by exact `FrameKey`;
- paces replay using recorded source/media timing;
- uses the same Raw/Overlay rendering path;
- does not substitute nearest metadata.

**Mandatory hardware validation:** confirm that remuxing H.264 into MKV preserves the project `user_data_unregistered` SEI through FFmpeg demux/decode so replayed `AVFrame` objects still expose the exact FrameIdentity.

---

## 8. Telemetry UI

PR #17 implements a third Vulkan/ImGui telemetry window.

Current model includes:

- five-minute bounded telemetry history;
- approximately 250 ms samples;
- bounded event log;
- RTSP connection/session/reconnect state;
- decoded/Raw/Overlay counters;
- missing-metadata drops;
- UDP packet/error/duplicate/gap telemetry;
- recorder queue/write/failure counters;
- queue depths;
- Raw/Overlay FPS history graphs;
- exact-synchronized current target table;
- session/reconnect/error event log.

Decode/UDP/recorder threads do not call ImGui/Vulkan directly.

The telemetry dashboard is currently primarily read-only. A richer settings/operator panel remains follow-up work.

---

## 9. Screenshots

PR #22 implements explicit lossless screenshots.

Controls:

- F11: next rendered Raw frame;
- F12: next exact Overlay frame including primitives/text.

The screenshot path copies the **final rendered swapchain image** into a host-visible staging buffer only after an explicit request.

Steady-state decode/render remains zero-copy.

Output is currently dependency-free 32-bit BMP.

---

## 10. Canonical stacked PR chain

As of this handoff, the canonical implementation chain is:

| PR | Branch | Purpose | Linux CI | Windows CI |
|---:|---|---|:---:|:---:|
| #1 | `mvp/phase-a-vulkan-probe` | Vulkan H.264 RTSP decode + Raw presentation | green | green |
| #9 | `mvp/frame-sync-foundation` | FrameKey, CV protocol, UDP metadata, exact synchronizer | green | green |
| #10 | `mvp/multiwindow-overlay-presentation` | Raw + delayed Overlay Vulkan windows | green | green |
| #11 | `mvp/overlay-primitives` | transform/history/primitive model | green | green |
| #12 | `mvp/overlay-imgui-vulkan` | actual Vulkan-only ImGui overlay drawing | green | green |
| #13 | `mvp/frame-identity-payload` | canonical 32-byte identity payload | green | green |
| #14 | `mvp/blackbox-recorder` | compressed H.264 rolling MKV recorder | green | green |
| #15 | `mvp/blackbox-metadata-journal` | rolling CV metadata journal | green | green |
| #16 | `mvp/watchdog-reconnect` | automatic RTSP reconnect + safe session teardown | green | green |
| #17 | `mvp/telemetry-window` | third-monitor Vulkan telemetry dashboard | green | green |
| #18 | `mvp/display-recovery` | monitor placement + per-window surface recovery | green | green |
| #19 | `mvp/replay-foundation` | frame-accurate Vulkan blackbox replay | green | green |
| #21 | `mvp/device-loss-recovery` | full in-process Vulkan device-loss rebuild | green | green |
| #22 | `mvp/screenshot-capture` | explicit Raw/Overlay screenshots | green | green |
| #23 | `mvp/h264-frame-id-sei` | canonical H.264 SEI NAL builder | green | green |

All of these remain draft because CI compilation/tests are not a substitute for target NVIDIA hardware validation.

### Superseded/alternate PRs

Do not use these as the continuation point:

- **#20 `mvp/frame-accurate-replay`** — alternate replay implementation; canonical replay line is #19.
- **#24 `mvp/display-hotplug`** — alternate/smaller display-hotplug implementation; canonical display/surface recovery line is #18.

Earlier experimental PRs #2-#8 also overlap functionality that was superseded by the canonical chain above.

---

## 11. What CI currently proves — and what it does not

Linux and Windows CI currently proves:

- cross-platform C++ compilation;
- Vulkan API/header compatibility at compile time;
- FFmpeg API compatibility;
- pure protocol/synchronization tests;
- overlay transform/model tests;
- recorder mux/journal tests;
- replay/index/clock tests;
- typed Vulkan error-classification tests;
- screenshot BMP regression tests;
- FrameIdentity payload/NAL golden tests.

CI does **not** prove:

- NVIDIA Vulkan Video decode works on the target driver;
- actual WSI behavior on Windows and Ubuntu Wayland/X11;
- decoded `AVVkFrame` layout/semaphore synchronization is valid on hardware;
- target latency;
- RTSP packet-loss behavior on the real LAN;
- multi-monitor hotplug behavior on target systems;
- real `VK_ERROR_DEVICE_LOST` recovery;
- SEI preservation through the real encoder -> RTSP -> Jetson/Viewer path;
- SEI preservation through MKV record/replay;
- 1080p60/120 throughput.

Do not call the MVP production-ready until those hardware gates are completed.

---

## 12. Recommended next implementation plan

The next work should be driven by integration evidence rather than adding more desktop features.

### P0 — integrate source FrameIdentity into the real UNIGINE H.264 encoder path

Goal: make every encoded source frame carry the canonical identity before doing more overlay work.

Tasks:

1. Identify the exact UNIGINE/encoder API boundary between rendered source frame and H.264 access unit.
2. Generate one `stream_epoch` per logical encoder session.
3. Assign monotonically increasing `frame_id` before encode.
4. Generate the SEI with `buildFrameIdentitySeiNal()`.
5. Insert it before the VCL NAL units of the same access unit.
6. Verify the same RTSP stream reaches both Viewer and Jetson with the SEI intact.
7. Add a source-side integration probe/log that prints epoch/frame ID and encoded AU sequence.

Acceptance:

- Viewer prints exact FrameIdentity on every decoded frame for a sustained stream;
- no identity drift/skips except dropped encoded frames;
- restart changes `stream_epoch`;
- `frame_id` monotonic inside one epoch.

### P0 — Jetson extraction + real CVM1 sender

Tasks:

1. Recover the exact source `FrameKey` from the decoded frame/SEI on Jetson.
2. Associate CV inference output with that exact key.
3. Serialize CVM1 using the documented layout.
4. Send one bounded UDP datagram per processed frame.
5. Exercise loss/duplicate/out-of-order behavior intentionally.
6. Confirm Viewer never draws mismatched metadata.

Acceptance:

- exact target boxes remain attached to the correct delayed frame while CV latency changes;
- duplicate/out-of-order datagrams do not corrupt correspondence;
- missing metadata causes only the expected overlay frame drop.

### P0 — hardware smoke validation on both desktop OSes

Run the canonical top branch on:

- Ubuntu 26.04 + Wayland;
- Ubuntu 26.04 + X11 if required in deployment;
- Windows 10/11;
- target NVIDIA hardware/driver.

Minimum gate:

- 1920x1080@60;
- 10+ minute sustained run;
- Vulkan validation clean;
- no unbounded latency growth;
- Raw latest-frame behavior verified;
- Overlay exact matching verified;
- recorder active;
- three windows active;
- monitor disconnect/reconnect tested;
- RTSP server interruption/recovery tested.

Record the results in a new `docs/HARDWARE_VALIDATION.md`.

### P0 — latency measurement and overlay-delay policy

Current 150 ms overlay delay is only a conservative initial value.

Instrument timestamps at:

- source render/frame assignment;
- encoder AU emission if available;
- Viewer packet receive;
- Viewer decode completion;
- Raw submit/present;
- Jetson decode/inference start/end;
- metadata receive;
- Overlay present.

Use source `FrameKey` to correlate measurements.

Then decide whether production behavior is:

1. fixed delay tuned to measured worst-case CV latency;
2. adaptive delay with bounded min/max;
3. operator-configurable fixed delay with telemetry warning when metadata regularly misses deadlines.

Exact-frame matching must remain unchanged regardless of delay policy.

### P1 — complete protocol support for non-bbox overlay data

The current CVM1 target packet is ideal for <=10 bbox-style targets but does not carry arbitrary polygons/masks.

Recommended design:

- keep CVM1 as the small per-frame target summary;
- introduce a versioned secondary payload/message for sparse geometry;
- introduce a separate bounded transport/representation for dense masks;
- reference all extensions by exact `FrameKey` and target ID.

Do not fragment a giant UDP CVM1 packet.

### P1 — replay validation and operator controls

Verify on NVIDIA hardware that recorded MKV retains FrameIdentity SEI after remux.

Then add operator replay controls as needed:

- pause/resume;
- frame step;
- seek/timeline;
- speed selection;
- jump to event/target;
- screenshot from replay.

All overlay lookup must remain exact FrameKey lookup.

### P1 — telemetry/settings UI

Add controlled settings to the existing telemetry window rather than placing controls over Raw.

Useful settings:

- overlay playout delay;
- overlay master alpha;
- bbox/trail style;
- metadata bind/port status;
- recorder enable/status/path/retention;
- monitor-role assignment;
- windowed/fullscreen behavior;
- target/class filters;
- screenshot/recording status.

Keep settings changes bounded and thread-safe.

### P1 — fullscreen and deployment policy

The canonical #18 path currently focuses on robust window/display recovery.

Decide and implement deployment policy:

- normal resizable windows;
- borderless fullscreen per monitor;
- startup role placement;
- behavior when only 1 or 2 monitors are present;
- persistent monitor selection by stable display identity if SDL/backend exposes a reliable identifier.

The alternate PR #24 contains a simpler CLI-oriented fullscreen/display-index experiment and can be mined for ideas, but should not become the canonical base.

### P1 — recorder hardening

Add:

- disk free-space telemetry;
- explicit recorder degraded/recovered events;
- configurable retention by both time and maximum disk bytes;
- crash-resilient partial-segment strategy;
- optional fsync policy;
- blackbox export/copy workflow so selected evidence is not rotated away.

### P2 — performance envelope

After 1080p60 correctness:

1. 1080p120;
2. 1440p60;
3. stress with maximum expected overlay primitives;
4. verify decode surface pressure;
5. tune `extra_hw_frames`;
6. measure Raw present mode behavior per platform/display;
7. measure CPU/GPU utilization with recorder + telemetry + overlay active.

Do not optimize by introducing decoded-frame CPU copies.

---

## 13. Known open decisions / risks

1. **Real RTSP fan-out topology is not finalized.** The source currently exposes one UDP/RTSP URL. Confirm whether Viewer and Jetson use independent RTSP sessions, multicast, or server-side duplication. Exact FrameIdentity makes correspondence independent of which delivery topology is chosen, but packet-loss behavior may differ.

2. **Actual CV worst-case latency is not measured yet.** The 100 ms expectation must be replaced by real percentile measurements.

3. **Overlay delay is currently fixed/configured, not adaptive.**

4. **CVM1 does not carry dense masks/polygons.** Protocol work is required before those can come from Jetson.

5. **UNIGINE/encoder SEI insertion is not integrated yet.** The byte-level builder is ready.

6. **Jetson sender implementation is not in this desktop repository unless added later.** The protocol/docs are ready for it.

7. **Hardware validation is still the largest risk.** Vulkan Video, FFmpeg `AVVkFrame` synchronization, WSI and device-loss semantics need actual NVIDIA testing.

8. **Replay SEI preservation must be verified on hardware/real recorded H.264.**

9. **Fullscreen/operator monitor assignment is not finalized in the canonical branch.**

10. **No claim has been made that 1080p120 is validated.**

---

## 14. Suggested next PR sequence

Continue from `mvp/h264-frame-id-sei`.

Suggested branches:

```text
integration/unigine-frame-identity
integration/jetson-metadata-e2e
validation/nvidia-hardware
mvp/adaptive-overlay-delay
mvp/overlay-geometry-protocol
mvp/replay-controls
mvp/operator-settings
mvp/recorder-hardening
```

Do not start all of these in parallel. The first three are the critical path because they produce evidence about real latency, SEI survival and hardware behavior.

---

## 15. Merge strategy

The current work is a stacked draft series.

Preferred order is exactly the canonical PR order in section 10.

Before merging:

1. ensure each PR base is still its intended previous branch;
2. ensure Linux and Windows CI are green on current heads;
3. run hardware validation against the top stack;
4. fix failures on the lowest relevant layer;
5. restack descendants if a parent branch head changes;
6. merge bottom-up.

Do not squash several architectural layers together before hardware debugging unless there is a deliberate reason; the current stack is useful for isolating failures.

---

## 16. New-chat bootstrap prompt

Copy this into the next ChatGPT coding conversation:

> Continue implementation of GitHub repository `kxsxlxv/Reg`.
>
> First read `docs/HANDOFF.md` from branch `mvp/h264-frame-id-sei`.
>
> The canonical stacked PR chain ends at PR #23. Do not continue from `master`, PR #20, or PR #24.
>
> Preserve these invariants: NVIDIA-only Vulkan, no OpenGL or fallback, no decoded-frame CPU roundtrip, one shared Vulkan H.264 decode path, Raw newest-frame-wins, Overlay exact `(stream_epoch, frame_id)` only, missing metadata => overlay frame drop, recorder disk I/O must not block live decode/render.
>
> Verify current GitHub PR/CI state before editing. Continue with the highest-priority unfinished item from the handoff plan, preferably real UNIGINE source FrameIdentity integration / Jetson end-to-end metadata integration / target NVIDIA hardware validation. Push all code and documentation changes to GitHub in a new stacked branch/PR.

---

## 17. Definition of MVP complete

The MVP should not be called complete until all of the following are demonstrated on target hardware:

- Ubuntu 26.04 and Windows 10/11 builds run;
- only NVIDIA/Vulkan hardware path is used;
- no OpenGL dependency/backend/fallback is active;
- no decoded-frame CPU roundtrip occurs during normal operation;
- Raw 1080p60 is stable with minimum practical latency;
- source FrameIdentity survives encoder/RTSP/decode;
- Jetson returns exact FrameKey metadata;
- Overlay remains exact under variable CV latency;
- missing metadata frames are dropped rather than mismatched;
- Cyrillic/UTF-8 labels render correctly;
- required bbox/line/trail/circle/crosshair/fill primitives render correctly;
- blackbox records compressed video + metadata for at least the configured retention;
- frame-accurate replay works from recorded data;
- screenshots work without affecting steady-state architecture;
- RTSP interruption reconnects automatically;
- monitor loss/reconnect recovers without application restart;
- Vulkan surface loss recovers per window;
- Vulkan device loss rebuilds the runtime in-process;
- telemetry/log UI remains responsive and bounded;
- 10+ minute validation shows no unbounded latency or memory growth;
- Vulkan validation shows no synchronization/lifetime errors.

