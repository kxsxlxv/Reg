# Jetson FrameIdentity probe

`reg_jetson_probe` is the P1 hardware gate for the Jetson path. It is deliberately headless: there is no SDL window, swapchain, renderer, overlay, recorder, or CV inference.

The validated path is:

```text
RTSP H.264
  -> FFmpeg
  -> NVIDIA Vulkan H.264 hardware decode
  -> AVFrame (AV_PIX_FMT_VULKAN)
  -> AV_FRAME_DATA_SEI_UNREGISTERED
  -> exact RGF1 FrameIdentity
```

The probe never creates a local frame ID and never uses timestamps, arrival order, nearest-frame matching, or any other fallback correspondence.

## Requirements

- NVIDIA Vulkan device, Vulkan 1.3 or newer.
- Vulkan Video H.264 decode support (`VK_KHR_video_queue`, `VK_KHR_video_decode_queue`, `VK_KHR_video_decode_h264`).
- FFmpeg build exposing the H.264 Vulkan hardware decoder path used by Reg.
- CMake 3.28+ and a C++23 compiler.

The target is a standalone CMake subproject so the Jetson smoke test does not require SDL, `glslc`, presentation support, or the full viewer build.

## Build

From the Reg repository root:

```bash
cmake -S tools/jetson_probe -B build-jetson-probe -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-jetson-probe --target reg_jetson_probe -j
```

If FFmpeg is installed in a non-system prefix, pass the same CMake/FFmpeg discovery settings used by the main Reg build.

## Run

The current realtime endpoint is the default, so the minimal hardware test is:

```bash
./build-jetson-probe/reg_jetson_probe --frames 300
```

Equivalent explicit form:

```bash
./build-jetson-probe/reg_jetson_probe \
  --url rtsp://192.168.50.1:8555/reg \
  --identity-probe-frames 300
```

Expected result:

```text
[jetson-probe] PASS decoded=300 identified=300 missing=0 non_monotonic=0 epoch_changes=0 target=300
```

`epoch_changes` may be nonzero if the source session changes while the probe is running. That is valid. Within one epoch, `frame_id` must strictly increase; gaps are allowed, duplicates and regressions are not.

Exit codes:

- `0`: PASS.
- `2`: FrameIdentity validation failure.
- `1`: initialization, RTSP, FFmpeg, Vulkan, or decode failure.

Do not proceed to synthetic CVM1 testing until this probe passes on the Jetson against the real render-machine RTSP stream.
