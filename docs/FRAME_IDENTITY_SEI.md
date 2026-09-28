# Source Frame Identity SEI v1

Reg requires an explicit source-frame identity so the Viewer and Jetson can correlate CV results without timestamp heuristics.

## Carrier

The identity is carried in H.264:

```text
SEI user_data_unregistered
```

The codec-specific adapter on the Render Machine is responsible for attaching one identity message to each source access unit that may be processed by Jetson/Viewer.

The Reg payload codec itself is independent of the encoder implementation.

## UUID

The 16-byte `user_data_unregistered` UUID is:

```text
7f 53 3b 8d 1a 91 4c 2d 9f 6a 52 45 47 46 49 44
```

ASCII in the final four bytes is `GFID`; implementations must use the complete binary UUID, not compare an ASCII suffix.

## Payload

Immediately after the UUID is a fixed v1 payload of **32 bytes**.

All integers use little-endian byte order.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | bytes | magic = `RGF1` |
| 4 | 2 | u16 | payload version = 1 |
| 6 | 2 | u16 | payload size = 32 |
| 8 | 8 | u64 | stream_epoch |
| 16 | 8 | u64 | frame_id |
| 24 | 8 | u64 | source_time_ns |

Canonical correspondence key:

```text
FrameKey = (stream_epoch, frame_id)
```

`source_time_ns` is diagnostic timing data only. Exact overlay matching must never depend on synchronized clocks.

## stream_epoch

`stream_epoch` identifies a logical source session.

It must change when a source restart or other discontinuity could cause frame IDs or encoded timing to restart.

A practical implementation may use a random/session-generated 64-bit value at encoder startup.

Viewer and Jetson copy the value from the video; they do not invent independent epochs.

## frame_id

`frame_id` is a monotonically increasing source-frame sequence within the epoch.

It must be assigned before encoding so the same visual frame has the same identity everywhere.

The Jetson metadata packet returns exactly this FrameKey.

Jetson must not use a local decoder-frame counter as a substitute.

## Golden payload example

For:

```text
stream_epoch   = 0x0102030405060708
frame_id       = 0x1122334455667788
source_time_ns = 987654321
```

the 32-byte payload is:

```text
52 47 46 31 01 00 20 00
08 07 06 05 04 03 02 01
88 77 66 55 44 33 22 11
b1 68 de 3a 00 00 00 00
```

This vector is enforced by `frame_identity_tests`.

## C++ helpers

The shared implementation exposes:

```cpp
encodeFrameIdentityPayload(identity)
decodeFrameIdentityPayload(payload)
extractFrameIdentity(avFrame)
```

The first two functions define the project wire contract.

`extractFrameIdentity()` additionally validates the SEI UUID when consuming FFmpeg `AV_FRAME_DATA_SEI_UNREGISTERED`.

## Encoder integration

Do not hand-build the payload layout in several places.

The UNIGINE/source integration should:

1. assign `stream_epoch` once per logical stream session;
2. increment `frame_id` for each rendered source frame;
3. call the canonical payload encoder, or implement the documented golden-compatible layout;
4. attach UUID + payload as H.264 `user_data_unregistered` SEI to that frame's access unit;
5. preserve the identity through the RTSP stream sent to both Viewer and Jetson.

The exact encoder API hook depends on the H.264 encoder/server implementation and is intentionally outside the portable Viewer core.
