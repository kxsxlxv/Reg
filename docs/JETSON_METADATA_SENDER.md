# Jetson CV metadata sender

This component is the intended Jetson Orin-side transport for Reg CV metadata.

It sends the same binary `CVM1` packets documented in
`docs/METADATA_PROTOCOL.md`.

## CMake targets

For transport only:

```cmake
target_link_libraries(your_cv_process PRIVATE reg_metadata_sender)
```

The sender target contains only:

- the CV metadata wire codec;
- the metadata sender;
- the native UDP transmit socket.

It does not depend on SDL, Dear ImGui, FFmpeg or the Viewer rendering path.

For the recommended decoded-frame integration:

```cmake
target_link_libraries(your_cv_process PRIVATE reg_jetson_metadata_bridge)
```

`reg_jetson_metadata_bridge` adds only FFmpeg `avutil` plus the shared
FrameIdentity decoder. It extracts the source-assigned Reg
`user_data_unregistered` SEI from the decoded `AVFrame` and carries that
exact identity through asynchronous inference completion.

It does not depend on SDL, Vulkan, Dear ImGui, or the Viewer rendering path.

## Recommended decoded-frame integration

```cpp
#include "jetson/MetadataBridge.hpp"

reg::jetson::MetadataBridge bridge(
    "192.168.1.50",
    50010);

// At the point where this decoded AVFrame is handed to CV:
auto context = bridge.beginFrame(
    decodedFrame,
    cvBeginNs);

if (!context) {
    // Required behavior: skip metadata for this frame.
    // Never replace the missing key with a local decoder/inference counter.
    return;
}

// Keep the small FrameContext with the asynchronous inference job.
// The AVFrame itself may be released after the CV pipeline has retained
// whatever image resources it needs.

reg::metadata::FrameMetadata result{};
result.flags = frameFlags;
result.targets = std::move(targets);

// At inference completion, even if frames finish out of order:
bridge.sendResult(
    *context,
    cvEndNs,
    std::move(result));
```

`beginFrame()` is the provenance boundary. It returns a context only when the
decoded frame contains a valid Reg FrameIdentity SEI. `sendResult()`
overwrites any `FrameMetadata.key`, `cvBeginNs`, and `cvEndNs` values with
the values bound to that context/completion. This prevents accidental use of a
Jetson-local frame counter as the correspondence key.

The context also exposes `sourceTimeNs()` for latency diagnostics. That
timestamp is never used for frame matching.

## Transport-only usage

```cpp
#include "metadata/MetadataSender.hpp"

reg::metadata::MetadataSender sender(
    "192.168.1.50",
    50010);

reg::metadata::FrameMetadata metadata{};
metadata.key.streamEpoch = streamEpochFromVideo;
metadata.key.frameId = frameIdFromVideo;
metadata.cvBeginNs = cvBeginNs;
metadata.cvEndNs = cvEndNs;

metadata.targets.push_back(
    reg::metadata::TargetMetadata{
        .id = targetId,
        .classId = classId,
        .flags = 0,
        .confidence = confidence,
        .bbox = {
            .x = normalizedX,
            .y = normalizedY,
            .width = normalizedWidth,
            .height = normalizedHeight,
        },
    });

const bool sent =
    sender.send(std::move(metadata));
```

The sender owns the independent UDP packet sequence number. Do not use that
sequence as the video frame identity.

The only video/metadata matching key is:

```text
(stream_epoch, frame_id)
```

Those values must be recovered from the corresponding source-video frame,
not generated independently on Jetson.

## Realtime behavior

The sender uses a connected, non-blocking UDP socket.

A frame send therefore has three relevant outcomes:

- valid packet + kernel accepted it: `send()` returns `true`;
- kernel transmit buffer would block: `send()` returns `false`;
- configuration/socket failure: exception.

A would-block result is intentionally a drop instead of backpressure into
the CV pipeline.

The packet sequence is incremented even when the OS would block. This means
the Viewer can observe the resulting sequence gap.

## Allocation behavior

`MetadataSender` uses a fixed 1400-byte stack buffer and
`encodeFrameMetadataInto()`.

The sender does not allocate a packet byte vector for each frame.

The target vector inside `FrameMetadata` remains application-owned normal
C++ data. A Jetson integration can reserve its expected maximum target count
once and reuse that capacity.

## Threading

One `MetadataSender` instance is intended to be used by one producer thread.

If several CV workers produce results concurrently, serialize completed
frame metadata onto one sender thread or give each independent sender its own
sequence domain. The preferred architecture is one ordered output stage so
packet sequence telemetry remains meaningful.

## Bounding boxes

Bounding boxes remain normalized source-video coordinates:

```text
x, y, width, height in [0,1]
origin = top-left
```

Do not convert them to monitor coordinates on Jetson.

The Viewer owns letterbox/crop/zoom/pan conversion.

## Integration tests

`metadata_sender_tests` creates a UDP receiver on an ephemeral loopback port,
sends a real CVM1 datagram through `MetadataSender`, decodes it, and verifies
the exact `FrameKey`, sequence and target payload.

The same test also verifies that the allocation-free encoder is byte-for-byte
identical to the existing vector-returning codec API.

`jetson_metadata_bridge_tests` additionally constructs decoded `AVFrame`
objects with the canonical Reg SEI side data and verifies the complete local
path:

```text
decoded AVFrame Reg SEI
    -> FrameContext
    -> variable/out-of-order inference completion
    -> MetadataBridge
    -> non-blocking UDP CVM1
    -> decode/validate exact FrameKey
```

The out-of-order case intentionally completes frame 101 before frame 100. UDP
packet sequence follows completion/send order while the CVM1 `FrameKey`
remains 101 then 100. This is the required behavior: transport order is
telemetry only and never substitutes for source frame identity.

These tests validate the software integration boundary. They do not replace
the mandatory Jetson hardware test proving that the real NVIDIA decode path
preserves the source SEI on decoded frames.
