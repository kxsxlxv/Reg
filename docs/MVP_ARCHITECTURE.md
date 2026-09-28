# Reg MVP Architecture

## Scope

Reg is a cross-platform NVIDIA-only H.264 RTSP viewer/recorder for Ubuntu 26.04 (Wayland/X11) and Windows 10/11.

The MVP has three logical outputs:

1. **Raw monitor** — minimum latency, latest decoded frame wins.
2. **Overlay monitor** — deliberately delayed video, exact frame-to-CV-metadata matching.
3. **Telemetry monitor** — logs, targets, event timeline, FPS/latency/network/GPU metrics.

The source is UNIGINE 2.22 SIM on an Ubuntu render machine. A Jetson Orin Nano 8 GB (JetPack 7.2 / CUDA 13.2) receives the same logical video, runs CV, and sends metadata back over UDP.

## Fixed technology decisions

- C++23
- CMake
- SDL3 for windows/events/Vulkan surfaces
- Vulkan 1.3+ on both Windows and Linux
- FFmpeg libavformat/libavcodec/libavutil
- H.264 hardware decoding through FFmpeg's Vulkan hwaccel
- `AV_PIX_FMT_VULKAN` decoded frames
- Dear ImGui + ImPlot in later UI phases, Vulkan backend only
- Matroska/MKV for compressed blackbox recording
- UDP binary CV metadata
- OpenGL is prohibited and must not be linked as a renderer/fallback

## Hard invariants

1. Normal live decoded video never performs GPU -> CPU -> GPU roundtrips.
2. Software video decode is not a fallback path.
3. Raw playback never accumulates a frame FIFO; newest frame wins.
4. Overlay metadata is applied only when `video.FrameKey == metadata.FrameKey`.
5. Missing overlay metadata at the playout deadline drops that video frame.
6. The viewer has one decode path shared by Raw and Overlay consumers.
7. Blackbox recording writes original compressed H.264 packets; it does not re-encode decoded frames.
8. Disk I/O must not indefinitely block stream/decode.
9. Swapchain/monitor failure must not automatically stop recording or stream receive.
10. `AVVkFrame` image layout, queue-family and timeline-semaphore state must be handled by one centralized synchronization component before rendering is introduced.

## Video path

```text
RTSP/RTP/UDP
    |
    v
FFmpeg libavformat
    |
    +---------------------------> compressed packet recorder (later)
    |
    v
FFmpeg H.264 Vulkan hwaccel
    |
    v
AV_PIX_FMT_VULKAN / AVVkFrame / VkImage
    |
    +--> RawFrameMailbox (newest only) --> Raw renderer
    |
    +--> bounded delayed frame buffer --> exact metadata synchronizer --> Overlay renderer
```

There must be no `av_hwframe_transfer_data()`, `sws_scale()`, host YUV conversion, or equivalent operation in the live display path.

## Vulkan ownership

The application creates and owns:

- `VkInstance`
- NVIDIA `VkPhysicalDevice` selection
- `VkDevice`
- graphics queue
- present queue
- H.264 video-decode queue
- swapchains

FFmpeg receives the same application-owned Vulkan objects through an `AV_HWDEVICE_TYPE_VULKAN` context created with:

```text
av_hwdevice_ctx_alloc()
manual AVVulkanDeviceContext population
av_hwdevice_ctx_init()
```

A second hidden FFmpeg Vulkan device is not permitted in the target architecture.

Required GPU properties for the MVP:

- NVIDIA vendor (`0x10DE`)
- Vulkan >= 1.3
- graphics and presentation
- `VK_KHR_video_queue`
- `VK_KHR_video_decode_queue`
- `VK_KHR_video_decode_h264`
- timeline semaphores
- synchronization2
- sampler YCbCr conversion
- dynamic rendering

## Raw policy

Raw uses a latest-value mailbox, not a FIFO:

```text
frame 100
frame 101
frame 102
renderer ready -> frame 102
```

Skipping obsolete Raw frames is expected behavior, not an error.

## Overlay policy

The initial overlay playout delay is 150 ms and must be configurable.

At 60 FPS, 150 ms is approximately 9 frames. At 120 FPS it is approximately 18 frames, so frame-pool sizing must be time/FPS-aware rather than based on a fixed 60 FPS assumption.

When the deadline for frame `N` arrives:

- exact metadata for frame `N` exists -> render `N + metadata(N)`;
- metadata is missing -> drop frame `N`;
- never reuse metadata from `N-1` or a nearest timestamp.

## Frame identity

The canonical key is:

```cpp
struct FrameKey {
    uint64_t streamEpoch;
    uint64_t frameId;
};
```

The preferred source transport is H.264 `user_data_unregistered` SEI. The source renderer/encoder embeds a versioned payload into each access unit. Both Viewer and Jetson recover the same key from video.

The current probe already contains a side-data parser for `AV_FRAME_DATA_SEI_UNREGISTERED`. The project UUID is defined in `src/media/FrameIdentity.hpp`.

The SEI payload is explicitly serialized and is **not** a packed native C++ struct. Version 1 payload after the 16-byte UUID is little-endian:

```text
byte 0..3   ASCII "RGF1"
byte 4..5   uint16 version = 1
byte 6..7   uint16 payload size = 32
byte 8..15  uint64 stream_epoch
byte 16..23 uint64 frame_id
byte 24..31 uint64 source_time_ns
```

## CV metadata transport (later phase)

Metadata uses UDP on the controlled LAN but must tolerate loss, duplicates and reorder. Production metadata must be binary/versioned, not JSON.

The packet header will carry at least:

- magic
- protocol version
- packet type
- byte size
- UDP metadata sequence number
- stream epoch
- frame ID
- CV begin/end timestamps
- object count
- flags
- CRC32C

The normal packet target is <= roughly 1200-1400 bytes to avoid IP fragmentation.

Target bbox coordinates should be normalized source-video coordinates. Viewer owns aspect-fit/fill, crop, zoom and pan transforms.

## Recording (later phase)

Compressed packets branch immediately after demux:

```text
AVPacket
  +--> decoder
  +--> recorder
```

No decode/re-encode is required. Use rolling MKV segments, initially targeting about five seconds and rotating at the next useful keyframe. Keep approximately five minutes. Metadata is stored in a versioned sidecar for frame-accurate replay.

The source encoder should use no B-frames and regular IDRs, initially about one IDR per second.

## Development phases

### Phase A.1 — implemented

- SDL3 Vulkan-only bootstrap window
- NVIDIA Vulkan 1.3 device selection
- graphics/present/video-decode queue selection
- application-owned `VkDevice`
- FFmpeg `AVVulkanDeviceContext` bound to that device
- RTSP/RTP UDP open
- H.264-only enforcement
- hardware decode with Vulkan-only `get_format`
- `AV_PIX_FMT_VULKAN` validation
- extra hardware surface configuration
- `VideoFrame` GPU-reference ownership
- SEI frame identity parsing
- latest-frame Raw mailbox

### Phase A.2 — implemented in source; hardware validation pending

The probe now renders the decoded `AVVkFrame` directly to a swapchain using Vulkan YCbCr sampling.

Implemented pieces:

- `Swapchain` with Raw-oriented IMMEDIATE -> MAILBOX -> FIFO preference;
- direct single-multiplanar NV12 `VkImage` sampling;
- `VkSamplerYcbcrConversion` for YCbCr matrix/range conversion;
- central `VulkanVideoFrameAccess` for FFmpeg timeline semaphore/layout state;
- `lock_frame()` / `unlock_frame()` around graphics submission;
- per-render-slot fences that retain `VideoFramePtr` until GPU completion;
- dynamic rendering and aspect-fit viewport;
- decode surface image-view/descriptor cache;
- swapchain recreation without intentionally stalling the decode queue;
- queue-handle isolation between FFmpeg and the application when internally synchronized queues are unavailable.

The current Phase A path deliberately rejects separate VkImage-per-plane output rather than inserting a conversion/copy.

The acceptance gate is documented in `docs/PHASE_A_SMOKE_TEST.md`.

### Later phases

- source SEI insertion + Jetson extraction
- fake delayed metadata synchronization tests
- real Jetson UDP metadata
- overlay primitives/text/history
- compressed blackbox + replay
- telemetry UI
- watchdog/reconnect/device-lost recovery
- measured latency tuning

## `AVVkFrame` synchronization rule

Rendering code must not take only `AVVkFrame::img[0]` and ignore FFmpeg state. It must honor:

- `layout[]`
- `access[]`
- `queue_family[]`
- `sem[]`
- `sem_value[]`
- `AVVulkanFramesContext::lock_frame()` / `unlock_frame()`

Every graphics submission that uses a decode image must wait for the frame timeline semaphore's current value and signal an incremented value. AVFrame references must remain alive until GPU completion, not merely until `vkQueueSubmit*()` returns.
