# Build and Probe Notes

## Required host components

- CMake >= 3.28
- C++23 compiler
- Vulkan SDK / Vulkan 1.3 development headers and loader
- NVIDIA driver with H.264 Vulkan Video decode support
- FFmpeg development libraries with Vulkan support (`avcodec`, `avformat`, `avutil`)

SDL3 is fetched by CMake by default and pinned to SDL 3.4.16. Its OpenGL/OpenGL ES options are forcibly disabled for this project.

## FFmpeg

The repository contains an FFmpeg submodule, but the first build integration deliberately links against an already-built FFmpeg installation. Building FFmpeg itself is platform-specific enough that it should not be hidden inside the main CMake configure step.

The intended release baseline is FFmpeg 9.x with Vulkan support. Set `FFMPEG_ROOT` when the libraries are not discoverable via pkg-config.

Expected tree:

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

## Configure

Linux example:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFFMPEG_ROOT=/opt/ffmpeg
cmake --build build -j
```

Windows example from a Developer PowerShell:

```powershell
cmake -S . -B build `
  -DFFMPEG_ROOT=C:/deps/ffmpeg
cmake --build build --config RelWithDebInfo
```

Use `-DREG_FETCH_SDL3=OFF` if SDL3 is provided by the package manager/toolchain.

## Run the Phase A probe

```bash
./build/reg_probe \
  --url rtsp://HOST/path \
  --max-delay-us 0 \
  --reorder-queue-size 0 \
  --extra-hw-frames 32
```

The initial probe validates the hardware decode path and logs Vulkan decoded frames. It does **not yet present the decoded image**; direct `AVVkFrame` -> swapchain rendering is Phase A.2 because FFmpeg timeline semaphore/image-layout ownership must be implemented before sampling the surfaces.

Expected output includes:

```text
[vulkan] GPU: NVIDIA ...
[vulkan] graphics queue family: ...
[vulkan] present queue family: ...
[vulkan] video decode queue family: ...
[rtsp] connected: ...
[decoder] opened H.264 decoder with extra_hw_frames=32
[decoder] frame=1 size=1920x1080 sei_frame_id=absent
```

If FFmpeg returns a software/CPU frame format, the application terminates. This is intentional.

## Latency options

`--max-delay-us` and `--reorder-queue-size` are exposed instead of being permanently buried in source code. `0/0` is the aggressive low-latency starting point, not a claim that it is always optimal. Deployment testing must compare it against a small RTP reorder/jitter allowance and measure corruption/drop behavior.
