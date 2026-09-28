# CV Metadata UDP Protocol v1

This document defines the wire format exchanged from the Jetson CV process to Reg.

The protocol is independent of C/C++ structure layout. Implementations must serialize each field explicitly.

## Transport

- UDP over the controlled LAN.
- One frame-metadata message per UDP datagram.
- Preferred datagram size: <= 1400 bytes.
- Reg v1 currently limits a frame message to 32 targets.
- Dense segmentation masks are not transported in this packet type.

## Byte order

All integer and IEEE-754 float fields are serialized in **little-endian** byte order.

Floating-point values are binary32 IEEE-754 and are transported by serializing their 32-bit bit pattern as little-endian uint32.

## Canonical frame identity

Metadata correspondence is defined only by:

```text
(stream_epoch, frame_id)
```

Do not match by wall-clock timestamp, UDP arrival time, RTP timestamp proximity, or a fixed frame offset.

`stream_epoch` changes when the logical source stream session changes.

`frame_id` is the source frame identity embedded in the encoded H.264 access unit and recovered by Jetson from that frame.

## FrameMetadata message

Magic:

```text
43 56 4d 31    "CVM1"
```

Message type:

```text
1 = FrameMetadata
```

Header size: **52 bytes**

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | bytes | magic = `CVM1` |
| 4 | 2 | u16 | protocol version = 1 |
| 6 | 2 | u16 | message type = 1 |
| 8 | 4 | u32 | complete datagram size including trailing CRC |
| 12 | 4 | u32 | UDP metadata packet sequence |
| 16 | 8 | u64 | stream_epoch |
| 24 | 8 | u64 | frame_id |
| 32 | 8 | u64 | Jetson CV begin timestamp, ns |
| 40 | 8 | u64 | Jetson CV end timestamp, ns |
| 48 | 2 | u16 | target_count |
| 50 | 2 | u16 | frame flags |

The timestamps are diagnostic fields. They do **not** participate in frame matching.

Cross-machine interpretation of these timestamps requires synchronized clocks. The synchronizer itself does not require clock synchronization.

## Target record

Each target record is exactly **32 bytes**.

| Offset within target | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | u64 | stable target ID |
| 8 | 2 | u16 | class ID |
| 10 | 2 | u16 | target flags |
| 12 | 4 | f32 | confidence, [0, 1] |
| 16 | 4 | f32 | bbox x |
| 20 | 4 | f32 | bbox y |
| 24 | 4 | f32 | bbox width |
| 28 | 4 | f32 | bbox height |

Bounding-box coordinates are normalized to the source image and are currently required to describe a rectangle inside `[0,1] x [0,1]`.

Origin:

```text
(0,0) = top-left
x grows right
y grows down
```

The Viewer performs source-video-to-screen conversion, including letterboxing, crop, zoom and pan.

Jetson must not transmit monitor pixel coordinates.

## CRC

The final four bytes of every v1 datagram contain CRC-32C / Castagnoli of all preceding bytes.

Parameters are equivalent to the common reflected CRC-32C implementation using polynomial:

```text
0x82F63B78
```

Known check vector:

```text
CRC32C("123456789") = 0xE3069283
```

## Datagram size

For `N` targets:

```text
size = 52 + N * 32 + 4
```

Examples:

```text
 0 targets =  56 bytes
10 targets = 376 bytes
32 targets = 1080 bytes
```

This remains below the configured 1400-byte application limit.

## Packet sequence semantics

`sequence` is independent from `frame_id`.

It is used to detect:

- duplicate metadata datagrams;
- lost metadata datagrams;
- reordered metadata datagrams.

It is a wrapping uint32 sequence number.

It must not be used to identify the corresponding video frame.

## Receiver validation

Reg rejects a frame-metadata packet when any of these checks fail:

- datagram shorter than the v1 minimum;
- bad magic;
- unsupported version;
- unsupported message type;
- declared size differs from received size;
- size exceeds the configured datagram limit;
- target count exceeds the v1 limit;
- target count does not agree with datagram size;
- CRC mismatch;
- non-finite confidence or geometry;
- confidence outside [0,1];
- invalid normalized bbox.

Rejected packets never enter the frame metadata store.

## Duplicate frame metadata

The metadata store is keyed by exact `FrameKey`.

If a second valid metadata packet arrives for the same:

```text
(stream_epoch, frame_id)
```

the first accepted packet wins and the later packet is counted as a duplicate.

## Overlay playout behavior

The Viewer keeps decoded video frames in a bounded GPU-backed history.

Initial configured playout delay:

```text
150 ms
```

At a buffered frame's deadline:

- exact matching metadata exists -> present frame + metadata;
- exact matching metadata does not exist -> drop that video frame.

The Viewer never substitutes metadata for another frame.

## Future extensions

New fields must be introduced through a new version or through a separately defined extensible payload.

Dense masks should use a separate transport rather than expanding this base datagram until IP fragmentation occurs.
