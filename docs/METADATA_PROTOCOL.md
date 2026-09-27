# CV Metadata Wire Protocol v1

## Purpose

This protocol carries frame-scoped CV metadata from Jetson to the Viewer over UDP.

Video and metadata are correlated only by:

```text
(stream_epoch, frame_id)
```

Arrival time, RTP timestamp proximity, and local frame counters are never used as substitutes for exact identity.

## Transport constraints

- Transport: UDP over the controlled LAN.
- Byte order: little-endian for every integer and IEEE-754 `float32`.
- One v1 frame-metadata message occupies one UDP datagram.
- Maximum accepted datagram size: 1400 bytes.
- Maximum targets per frame: 32.
- Normal expected target count: <= 10.
- IP fragmentation should be avoided.
- UDP loss, duplication, and reordering are expected and must not corrupt synchronization state.

## Frame metadata packet

Fixed header size: 52 bytes.

| Offset | Size | Field | Type |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | bytes `C V M 1` |
| 4 | 2 | version | `uint16`, currently 1 |
| 6 | 2 | message_type | `uint16`, FrameMetadata = 1 |
| 8 | 4 | packet_size | `uint32`, complete datagram size including CRC |
| 12 | 4 | sequence | `uint32`, metadata packet sequence |
| 16 | 8 | stream_epoch | `uint64` |
| 24 | 8 | frame_id | `uint64` |
| 32 | 8 | cv_begin_ns | `uint64` |
| 40 | 8 | cv_end_ns | `uint64` |
| 48 | 2 | target_count | `uint16` |
| 50 | 2 | flags | reserved, zero in v1 |

The header is followed by `target_count` fixed-size target records and a final CRC32C.

Total v1 packet size is:

```text
52 + target_count * 32 + 4
```

For 10 targets this is 376 bytes.

## Target record

Fixed target size: 32 bytes.

Offsets below are relative to the start of the target record.

| Offset | Size | Field | Type |
| ---: | ---: | --- | --- |
| 0 | 8 | id | `uint64` |
| 8 | 2 | class_id | `uint16` |
| 10 | 2 | flags | `uint16` |
| 12 | 4 | confidence | IEEE-754 `float32` |
| 16 | 4 | x | normalized `float32` |
| 20 | 4 | y | normalized `float32` |
| 24 | 4 | width | normalized `float32` |
| 28 | 4 | height | normalized `float32` |

Bounding boxes are expressed in source-video normalized coordinates.

Coordinate convention:

```text
(0,0) -----------------> +X
  |
  |
  v
 +Y

top-left = (x, y)
bottom-right = (x + width, y + height)
```

For v1, coordinates are expected to remain within the source image. The decoder accepts a very small tolerance for float rounding but rejects materially out-of-range boxes.

`confidence` must be finite and in `[0, 1]`.

All target floating-point fields must be finite. NaN and infinity are rejected.

## Packet sequence

`sequence` is independent of `frame_id`.

Its purpose is transport diagnostics:

- detect missing metadata datagrams,
- detect duplicates,
- detect packet reordering.

It must not be used to match metadata to video.

## Stream epoch

`stream_epoch` distinguishes separate logical source sessions.

A reconnect or source restart creates a new epoch.

Therefore:

```text
(epoch=10, frame=500)
```

and:

```text
(epoch=11, frame=500)
```

are unrelated frames.

Late packets from an old epoch must never be applied to a frame in the current epoch.

## CV timing

`cv_begin_ns` and `cv_end_ns` are Jetson-side timestamps.

If both are non-zero:

```text
cv_end_ns >= cv_begin_ns
```

must hold.

These fields are diagnostic/telemetry fields. Exact Viewer synchronization does not depend on cross-machine clock synchronization.

For true end-to-end latency decomposition, synchronize source, Jetson, and Viewer clocks separately (PTP is the preferred future mechanism).

## CRC32C

The final four bytes contain CRC32C (Castagnoli), little-endian.

CRC covers every packet byte before the CRC itself:

```text
CRC32C(packet[0 .. packet_size-5])
```

Parameters correspond to the common reflected CRC32C form:

- polynomial: `0x82F63B78`,
- initial value: `0xFFFFFFFF`,
- final XOR: `0xFFFFFFFF`.

Reference test vector:

```text
"123456789" -> 0xE3069283
```

Packets with an invalid CRC are discarded before entering `MetadataStore`.

## Validation order

The Viewer validates, in order:

1. minimum/maximum datagram length,
2. magic,
3. protocol version,
4. message type,
5. declared packet size,
6. target-count limit,
7. size implied by target count,
8. CRC32C,
9. CV timing,
10. target numeric/range validity.

No unchecked network length is used for an allocation.

## Duplicate and out-of-order behavior

Metadata is stored by exact `FrameKey`.

A duplicate packet for a FrameKey already present in the store is ignored by the current MVP policy.

Out-of-order packets are valid and remain independently addressable.

When an Overlay frame reaches its playout deadline:

- exact metadata present -> present video + metadata,
- exact metadata absent -> drop the video frame,
- stale/previous/nearest metadata -> never substitute.

## Evolution

Any incompatible wire change requires a new protocol version.

Do not change compiler struct packing and call that a protocol update. Serialization is explicit and must remain independent of host ABI, compiler padding, and native endianness.

Large dense segmentation masks are intentionally outside this datagram. They require a separate optional transport if introduced later.
