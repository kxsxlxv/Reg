# CV Metadata Protocol V1

## Purpose

This protocol carries frame-scoped CV results from Jetson to the Viewer over UDP.

Video and metadata are correlated only by:

    (stream_epoch, frame_id)

Arrival time, RTP timestamp proximity, packet order, and a fixed frame offset are not valid correlation mechanisms.

## Transport

- UDP over the controlled LAN.
- One V1 FrameMetadata message fits in one UDP datagram.
- All integer and floating-point fields are serialized explicitly.
- Wire byte order is little-endian.
- Floating-point fields are IEEE-754 binary32 bit patterns serialized as little-endian uint32 values.
- The current V1 limit is 32 targets per frame.
- Maximum V1 FrameMetadata datagram size is 1080 bytes.

Do not IP-fragment one logical V1 FrameMetadata message.

## Important semantic rule

Jetson must send one FrameMetadata packet for every video frame for which Overlay presentation is expected.

If a processed frame contains no detected targets, Jetson must still send a valid packet with:

    object_count = 0

No packet means the Viewer has no exact result for that frame. When the configured playout deadline expires, the Viewer drops that Overlay video frame.

## Frame identity

Both Viewer and Jetson obtain the source frame identity from the encoded video, preferably from the project H.264 user_data_unregistered SEI payload.

The metadata packet repeats:

- stream_epoch
- frame_id

Jetson must never substitute a local decode counter.

## Packet layout

FrameMetadata V1:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 4 | bytes | ASCII magic CVM1 |
| 4 | 2 | uint16 | version = 1 |
| 6 | 2 | uint16 | packet_type = 1 (FrameMetadata) |
| 8 | 4 | uint32 | total packet size including CRC |
| 12 | 4 | uint32 | UDP metadata sequence |
| 16 | 8 | uint64 | stream_epoch |
| 24 | 8 | uint64 | frame_id |
| 32 | 8 | uint64 | cv_begin_ns |
| 40 | 8 | uint64 | cv_end_ns |
| 48 | 2 | uint16 | object_count |
| 50 | 2 | uint16 | frame flags |
| 52 | N*32 | bytes | TargetV1 array |
| final 4 | 4 | uint32 | CRC32C |

Header size before targets: 52 bytes.

CRC32C covers every byte from offset 0 through the end of the target array. The CRC field itself is not included in the CRC input.

CRC polynomial is Castagnoli CRC32C. The standard test vector ASCII "123456789" must produce:

    0xE3069283

## TargetV1 layout

Each target occupies exactly 32 bytes:

| Target offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | uint64 | target ID |
| 8 | 2 | uint16 | class ID |
| 10 | 2 | uint16 | target flags |
| 12 | 4 | float32 | confidence |
| 16 | 4 | float32 | bbox x |
| 20 | 4 | float32 | bbox y |
| 24 | 4 | float32 | bbox width |
| 28 | 4 | float32 | bbox height |

## Bounding-box coordinates

Bounding boxes use normalized source-video coordinates.

Convention:

- origin: top-left of the source video;
- +X: right;
- +Y: down;
- x, y, width, height are each in [0, 1].

Coordinates are independent of:

- monitor resolution;
- letterboxing;
- window size;
- zoom;
- pan.

The Viewer owns source-to-screen transformation.

## Confidence

confidence must be finite and in [0, 1].

NaN and infinity are invalid protocol data and the packet is rejected.

## CV timestamps

cv_begin_ns and cv_end_ns are Jetson-side timestamps.

Requirements:

- both are uint64 nanoseconds in the same Jetson clock domain;
- if both are non-zero, cv_end_ns must be >= cv_begin_ns;
- their difference can be used for inference/CV processing duration.

Do not directly compare Jetson absolute timestamps with Viewer timestamps unless clocks are explicitly synchronized, for example with PTP.

The synchronizer itself does not depend on clock synchronization.

## Packet sequence

sequence is a uint32 datagram sequence used for telemetry:

- gap observation;
- duplicate detection;
- reorder observation.

It is not a frame identifier.

Correctness is determined only by stream_epoch + frame_id.

Sequence wrap is allowed.

## Loss, duplication, and reorder

The Viewer tolerates:

- packet loss;
- duplicated UDP packets;
- reordered UDP packets.

A valid reordered packet is still stored by FrameKey and can be matched later if its video frame has not reached the playout deadline.

A duplicate packet does not create a second metadata result.

Conflicting packets carrying the same FrameKey but different metadata sequence are treated as a protocol/data conflict.

## Target history

Jetson should normally transmit current target state, not the full trajectory history every frame.

The Viewer maintains target history locally by target ID.

## Dense masks

Dense pixel masks are not part of FrameMetadata V1.

If dense masks are required later, use a separate optional transport/message type or a compact representation such as contour/RLE/downscaled mask. Do not inflate the normal frame metadata datagram.

## Parser validation

The Viewer rejects a packet if any of the following is true:

- packet smaller than the minimum V1 size;
- packet larger than the V1 maximum;
- wrong magic;
- unsupported version;
- unsupported packet type;
- declared size differs from received datagram size;
- object_count exceeds 32;
- target-array size does not match object_count;
- CRC32C mismatch;
- invalid CV timing;
- non-finite float;
- confidence outside [0, 1];
- bbox component outside [0, 1].

Network lengths are validated before target-driven allocation.

## Reference implementation

Viewer implementation:

- src/metadata/Protocol.hpp
- src/metadata/Protocol.cpp

Core tests:

- tests/MetadataProtocolTests.cpp
