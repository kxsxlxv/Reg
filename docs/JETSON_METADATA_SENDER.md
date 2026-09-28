# Jetson CV metadata sender

This component is the intended Jetson Orin-side transport for Reg CV metadata.

It sends the same binary `CVM1` packets documented in
`docs/METADATA_PROTOCOL.md`.

## CMake target

```cmake
target_link_libraries(your_cv_process PRIVATE reg_metadata_sender)
```

The sender target contains only:

- the CV metadata wire codec;
- the metadata sender;
- the native UDP transmit socket.

It does not depend on SDL, Dear ImGui, FFmpeg or the Viewer rendering path.

## Basic usage

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

## Integration test

`metadata_sender_tests` creates a UDP receiver on an ephemeral loopback port,
sends a real CVM1 datagram through `MetadataSender`, decodes it, and verifies
the exact `FrameKey`, sequence and target payload.

The same test also verifies that the allocation-free encoder is byte-for-byte
identical to the existing vector-returning codec API.
