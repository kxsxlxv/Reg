# Reg

Reg is an NVIDIA-only, low-latency H.264 RTSP viewer/recorder with frame-accurate CV overlays.

Targets:

- Ubuntu 26.04 (Wayland and X11)
- Windows 10/11
- Vulkan 1.3+
- FFmpeg Vulkan H.264 hardware decode
- no OpenGL
- no decoded-frame CPU roundtrip

The architecture separates two presentation policies:

- **Raw:** newest decoded frame wins; minimize latency.
- **Overlay:** hold a short bounded GPU history and match Jetson CV metadata only by exact `(stream_epoch, frame_id)`.
- **Telemetry:** independent low-rate Vulkan/ImGui dashboard for logs, targets and runtime metrics.

## Current implementation state

The MVP is currently developed as a stacked draft-PR chain. The canonical continuation point is:

```text
branch: mvp/h264-frame-id-sei
PR:     #23
```

Do **not** treat `master` as the current implementation tip.

The current top stack includes:

- application-owned NVIDIA Vulkan device shared with FFmpeg;
- H.264 -> `AV_PIX_FMT_VULKAN` hardware decode;
- direct Vulkan YCbCr sampling with no decoded-frame CPU roundtrip;
- newest-frame-only Raw presentation;
- exact delayed Overlay synchronization by source `FrameKey`;
- binary CRC32C UDP CV metadata protocol;
- Vulkan-only Dear ImGui overlay primitives and UTF-8/Cyrillic-capable labels;
- separate Raw, Overlay and Telemetry Vulkan windows;
- rolling compressed-H.264 MKV blackbox recording;
- rolling CV metadata journals;
- frame-accurate Vulkan replay foundation;
- automatic RTSP reconnect;
- monitor/surface recovery;
- full in-process Vulkan device-loss rebuild;
- explicit one-shot Raw/Overlay screenshots;
- canonical H.264 FrameIdentity payload and SEI NAL builder.

Linux and Windows compile/test CI are green on the canonical top stack.

Target NVIDIA hardware validation and real UNIGINE/Jetson end-to-end integration are still mandatory before calling the MVP complete.

## Start here

For continuation, architecture invariants, canonical PR order, unfinished work and the next implementation plan:

- [Engineering handoff and next plan](docs/HANDOFF.md)
- [MVP architecture](docs/MVP_ARCHITECTURE.md)
- [Build notes](docs/BUILD.md)
- [Phase A hardware smoke test](docs/PHASE_A_SMOKE_TEST.md)
- [CV metadata protocol](docs/METADATA_PROTOCOL.md)
- [Source FrameIdentity SEI](docs/FRAME_IDENTITY_SEI.md)

## Core live path

```text
RTSP/RTP/UDP
    -> FFmpeg H.264 Vulkan decode
    -> AV_PIX_FMT_VULKAN / AVVkFrame
    -> shared GPU frame lifetime
       -> Raw: newest-frame-wins -> Vulkan swapchain
       -> Overlay: bounded delayed GPU history
          + exact Jetson metadata FrameKey match
          -> Vulkan video + ImGui/Vulkan primitives
```

Recording consumes the original compressed H.264 packets before decode and therefore does not add a decoded-frame readback/re-encode path.
