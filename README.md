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

The repository is currently implementing **Phase A**. `reg_probe` creates the application-owned Vulkan device, provides that same device to FFmpeg, opens H.264 RTSP/RTP over UDP, and rejects any decoder output other than `AV_PIX_FMT_VULKAN`.

See:

- [`docs/MVP_ARCHITECTURE.md`](docs/MVP_ARCHITECTURE.md)
- [`docs/BUILD.md`](docs/BUILD.md)

The next implementation slice is direct synchronized `AVVkFrame` YCbCr sampling into the Vulkan swapchain.
