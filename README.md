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
- **Overlay:** hold a short bounded GPU history and match Jetson CV metadata by explicit `(stream_epoch, frame_id)`.

## Current state

The `mvp/phase-a-vulkan-probe` branch contains the Phase A direct video path:

```text
RTSP/RTP/UDP
    -> FFmpeg H.264 Vulkan decode
    -> AV_PIX_FMT_VULKAN / AVVkFrame
    -> Vulkan YCbCr sampling
    -> Vulkan swapchain
```

The application creates the Vulkan device itself and gives that same device to FFmpeg. The renderer honors FFmpeg's per-frame timeline semaphore/layout state, keeps decoded surfaces alive until GPU completion, and never transfers decoded pixels through CPU RAM.

Raw presentation already uses newest-frame-only semantics.

Phase A source implementation is ready for the mandatory hardware smoke test. It has not been declared hardware-validated from this development environment.

A stacked Phase C/D branch now also contains the frame-accurate metadata control plane:

```text
Jetson UDP
    -> binary V1 validation + CRC32C
    -> MetadataStore(FrameKey)
                         ^
                         |
decoded SEI-tagged frame -> 150 ms bounded OverlayFrameBuffer
                         |
                         v
                  exact FrameSynchronizer
                  Present or Drop
```

When `--metadata-port` is enabled, `reg_probe` logs exact synchronized frames and deadline drops. It does not yet draw the second-monitor overlay.

See:

- [MVP architecture](docs/MVP_ARCHITECTURE.md)
- [Build notes](docs/BUILD.md)
- [Phase A hardware smoke test](docs/PHASE_A_SMOKE_TEST.md)
- [CV metadata protocol V1](docs/METADATA_PROTOCOL_V1.md)

The next visual milestone is a second Vulkan output with bbox/line/text overlay rendering. Source SEI insertion and Jetson extraction still need to use the FrameKey contract documented by the project.
