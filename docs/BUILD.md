# Build and Probe Notes

## Required host components

- CMake >= 3.28
- C++23 compiler
- Vulkan SDK / Vulkan 1.3 development headers and loader
- Vulkan SDK `glslc`
- NVIDIA driver with H.264 Vulkan Video decode support
- FFmpeg 9.x development libraries with Vulkan support: `avcodec`, `avformat`, `avutil`

SDL3 is fetched by CMake by default and pinned by the project. Its OpenGL/OpenGL ES options are forcibly disabled. Reg creates only Vulkan windows/surfaces.

## FFmpeg

The repository contains an FFmpeg submodule, but the current build links against an already-built FFmpeg installation. Set `FFMPEG_ROOT` when the libraries are not discoverable through pkg-config.

Expected layout:

```text
FFMPEG_ROOT/
  include/
    libavcodec/
    libavformat/
    libavutil/
  lib/
    avcodec...
    avformat...
    avutil...
```

FFmpeg must expose the Vulkan hardware API used by the probe, including `AV_PIX_FMT_VULKAN`, `AVVulkanDeviceContext`, `AVVulkanFramesContext`, and `AVVkFrame`.

## Configure

Linux:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFFMPEG_ROOT=/opt/ffmpeg \
  -DREG_ENABLE_VALIDATION=ON

cmake --build build -j
```

Windows, Developer PowerShell:

```powershell
cmake -S . -B build `
  -DFFMPEG_ROOT=C:/deps/ffmpeg `
  -DREG_ENABLE_VALIDATION=ON

cmake --build build --config RelWithDebInfo
```

Use `-DREG_FETCH_SDL3=OFF` if SDL3 is supplied by the package manager/toolchain.

CMake compiles `shaders/video.vert` and `shaders/video.frag` to SPIR-V using `glslc`.

## Run the Phase A probe

```bash
./build/reg_probe \
  --url rtsp://HOST/path \
  --max-delay-us 0 \
  --reorder-queue-size 0 \
  --extra-hw-frames 32
```

The probe performs the complete Phase A path: RTSP demux, H.264 Vulkan decode, direct YCbCr sampling from the decoded `VkImage`, and presentation to the swapchain.

Expected output includes:

```text
[vulkan] GPU: NVIDIA ...
[vulkan] graphics queue family: ...
[vulkan] present queue family: ...
[vulkan] video decode queue family: ...
[vulkan] graphics queue index: ...
[rtsp] connected: ...
[decoder] opened H.264 decoder with extra_hw_frames=32
[decoder] frame=1 size=1920x1080 sei_frame_id=absent
[renderer] presented=1 source=1920x1080 output=...
```

If FFmpeg returns a software/CPU frame format, the application terminates. This is intentional.

Phase A currently accepts an NV12-compatible single multiplanar Vulkan image. It deliberately fails instead of inserting a hidden conversion/copy when FFmpeg exposes a different surface representation.

## Queue ownership

FFmpeg receives queue index 0 for the Vulkan queue families exposed through `AVVulkanDeviceContext`.

If `VK_KHR_internally_synchronized_queues` is available, Reg enables it and passes matching queue creation flags to FFmpeg.

Otherwise, Reg requests a second queue from any queue family shared with renderer/presentation work and uses queue index 1 on the application side. If the required second queue is unavailable, initialization fails rather than creating a cross-thread `VkQueue` race.

## Latency options

`--max-delay-us` and `--reorder-queue-size` are exposed instead of being permanently buried in source. `0/0` is the aggressive low-latency starting point, not a claim that it is always optimal. Deployment testing must compare it against a small RTP reorder/jitter allowance.

## Validation gate

Follow [PHASE_A_SMOKE_TEST.md](PHASE_A_SMOKE_TEST.md) before starting the frame-accurate Overlay implementation.
