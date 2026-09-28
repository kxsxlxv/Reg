# Phase A Hardware Smoke Test

Phase A is accepted only after the direct hardware path has been exercised on a real NVIDIA system. Source review alone is not sufficient because the critical path depends on the NVIDIA Vulkan Video driver, FFmpeg Vulkan H.264 decode, and WSI.

## Preconditions

- NVIDIA GPU with Vulkan Video H.264 decode support.
- Current NVIDIA driver.
- Vulkan SDK with glslc.
- FFmpeg 9.x development build with Vulkan support.
- H.264 RTSP/RTP-over-UDP source.
- Source decodes to an NV12-compatible Vulkan hardware surface.

The probe intentionally fails instead of falling back to CPU decode or CPU color conversion.

## Build

Ubuntu:

    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFFMPEG_ROOT=/opt/ffmpeg \
      -DREG_ENABLE_VALIDATION=ON
    cmake --build build -j

Windows, Developer PowerShell:

    cmake -S . -B build `
      -DFFMPEG_ROOT=C:/deps/ffmpeg `
      -DREG_ENABLE_VALIDATION=ON
    cmake --build build --config RelWithDebInfo

## Run

    ./build/reg_probe \
      --url rtsp://HOST/path \
      --max-delay-us 0 \
      --reorder-queue-size 0 \
      --extra-hw-frames 32

On a multi-config Windows build, use the executable from the selected configuration directory.

## Expected startup

The process must print the NVIDIA GPU, graphics/present/video queue families and indices, RTSP connection information, H.264 Vulkan decoder initialization, decoded-frame count, and presented-frame count. The window must display live video.

If the source does not yet inject the Reg SEI payload, sei_frame_id=absent is expected.

## Hard pass conditions

1. Every accepted decoded frame is AV_PIX_FMT_VULKAN.
2. No decoded-frame CPU transfer occurs.
3. Vulkan validation reports no synchronization, image-layout, semaphore, descriptor-lifetime, or object-lifetime errors.
4. Video is visibly correct and is not obviously double-gamma encoded.
5. Aspect-fit and letterboxing remain correct while resizing.
6. Minimize/restore and swapchain recreation do not kill RTSP decode.
7. 1920x1080@60 runs for at least 10 minutes without unbounded latency growth.
8. Raw rendering skips obsolete frames under pressure instead of accumulating a FIFO.
9. Closing the window interrupts a blocked RTSP read and exits cleanly.

## Queue synchronization gate

The startup must use one of two safe arrangements:

- Internally synchronized queues: VK_KHR_internally_synchronized_queues is enabled and FFmpeg receives the same queue creation flags.
- Separate queue handles: when that extension is unavailable, FFmpeg uses queue index 0 for exposed queue families and renderer/presentation work uses queue index 1 for overlapping families.

Concurrent FFmpeg and application submissions to the same externally synchronized VkQueue handle are not allowed.

## Color check

Phase A uses Vulkan YCbCr conversion for matrix/range conversion and prefers an UNORM swapchain paired with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR. This prevents an sRGB color attachment from automatically encoding already non-linear video RGB values a second time.

The source should eventually carry explicit H.264 VUI color metadata. For the current SDR probe, an unspecified matrix falls back to BT.709 and unspecified range falls back to limited range. Exact transfer-function/color-management conversion is deferred beyond Phase A.

## Latency experiments after correctness

After validation passes, compare at least:

- max_delay=0, reorder_queue_size=0;
- a small RTP reorder queue;
- a small non-zero max_delay.

Measure latency, packet/decode errors, corrupted-frame frequency, and Raw presented/skipped frame counts. Do not choose the production jitter policy solely from theoretical minimum buffering.

## 120 FPS follow-up

After 1080p60 is stable, repeat with 1080p120 where source and display support it. If decoder-surface pressure appears, the first tuning parameter is --extra-hw-frames; start around 48.

## Failure report data

Capture OS/display server, GPU, driver, Vulkan SDK/header version, FFmpeg version/configure flags, full startup log, validation output, source H.264 profile/resolution/FPS/VUI metadata, and the exact stage where failure starts.
