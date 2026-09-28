# Reg Metadata UDP Protocol v1

This document defines the Viewer <-> Jetson frame-metadata wire format used by the MVP.

The protocol is intentionally small, fixed-layout, versioned, and independent of C/C++ structure padding.

## Transport

- UDP over the controlled LAN.
- One metadata datagram describes one source video frame.
- Normal packets should remain below 1400 bytes.
- The current implementation allows at most 32 targets per frame.
- The expected deployment load is approximately 10 targets per frame.

All integer and IEEE-754 floating-point fields are encoded in **network byte order (big-endian)**.

The final four bytes are CRC32C (Castagnoli) over every preceding byte of the datagram.

## Canonical frame identity

Every packet contains:

- `stream_epoch` — 64-bit logical stream-session identifier.
- `frame_id` — 64-bit source-frame identifier.

The pair:

```text
(stream_epoch, frame_id)
```

is the only canonical key used for Viewer/Jetson video-overlay matching.

Do not infer correspondence from arrival time, RTP timestamp proximity, or a fixed frame offset.

## Header

The v1 header is 52 bytes.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `CVM1` |
| 4 | 2 | protocol version, currently `1` |
| 6 | 2 | packet type, `1 = FrameMetadata` |
| 8 | 4 | total packet size including CRC32C |
| 12 | 4 | UDP metadata sequence number |
| 16 | 8 | `stream_epoch` |
| 24 | 8 | `frame_id` |
| 32 | 8 | Jetson CV begin timestamp, ns |
| 40 | 8 | Jetson CV end timestamp, ns |
| 48 | 2 | target count |
| 50 | 2 | packet flags |

Cross-machine timestamps are diagnostic only unless machine clocks are synchronized. They are never used for frame matching.

## Target record

Each target is exactly 32 bytes.

| Offset within target | Size | Field |
|---:|---:|---|
| 0 | 8 | target ID |
| 8 | 2 | class ID |
| 10 | 2 | target flags |
| 12 | 4 | confidence, float32 |
| 16 | 4 | normalized bbox x |
| 20 | 4 | normalized bbox y |
| 24 | 4 | normalized bbox width |
| 28 | 4 | normalized bbox height |

The MVP requires confidence and normalized bbox components to be finite values in `[0, 1]`.

Coordinates are relative to the source video image, not the monitor.

## Datagram size

The exact packet size is:

```text
52 + target_count * 32 + 4
```

For ten targets:

```text
52 + 10 * 32 + 4 = 376 bytes
```

This remains comfortably below a typical Ethernet MTU.

## Sequence number

`packet_sequence` is distinct from `frame_id`.

It is used to diagnose:

- duplicate datagrams,
- out-of-order delivery,
- observed sequence gaps.

The Viewer still matches overlay data only with `(stream_epoch, frame_id)`.

## Late packets

Once the Overlay synchronizer has presented or dropped a frame, the Viewer advances a metadata watermark.

Any metadata packet whose FrameKey is at or behind that watermark is classified as late and is not inserted into the live MetadataStore.

It may still be logged or recorded later for diagnostics.

## Duplicate policy

- Duplicate UDP sequence numbers are ignored by the live receiver.
- Multiple metadata packets for the same FrameKey are considered duplicate frame metadata.
- The first accepted packet for a FrameKey wins in the MVP.

## Invalid packets

The Viewer rejects packets with:

- wrong magic,
- unsupported version,
- unsupported packet type,
- declared-size mismatch,
- target count above 32,
- datagram size above 1400 bytes,
- CRC32C mismatch,
- NaN/infinite/out-of-range normalized target values.

Network-controlled sizes must not be used for unchecked allocation.

## CRC32C

Polynomial: Castagnoli.

Reference vector:

```text
CRC32C("123456789") = 0xE3069283
```

The test suite validates this value.

## Future extensions

Future schema versions may add:

- polygons,
- optional velocity,
- additional target attributes,
- heartbeat/status messages,
- event packets.

Dense segmentation masks must use a separate transport or compact representation; they must not inflate the normal frame-metadata datagram.
