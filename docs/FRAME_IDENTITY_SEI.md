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
buildFrameIdentitySeiNal(identity, framing)
extractFrameIdentity(avFrame)
```

The first two functions define the project payload wire contract.

`buildFrameIdentitySeiNal()` constructs a complete H.264 SEI NAL and performs H.264 emulation-prevention insertion. It supports:

- Annex-B framing: `00 00 00 01 | NAL(type=6) | EBSP`;
- AVCC framing with a 4-byte big-endian NAL length.

`extractFrameIdentity()` additionally validates the SEI UUID when consuming FFmpeg `AV_FRAME_DATA_SEI_UNREGISTERED`.

## Encoder integration

Do not hand-build the payload layout in several places.

The source integration must:

1. assign `stream_epoch` once per logical stream session;
2. increment `frame_id` for each encoded source access unit that is published;
3. call `buildFrameIdentitySeiNal()` or the shared Annex-B access-unit injector;
4. insert the returned SEI NAL into the **same H.264 access unit**, before that frame's VCL NAL units;
5. preserve the augmented bitstream through the RTSP stream sent to both Viewer and Jetson.

The builder emits SEI payload type 5 (`user_data_unregistered`), payload size 48 bytes (16-byte UUID + 32-byte Reg payload), RBSP trailing bits, and all required emulation-prevention bytes.

For Annex-B encoded frames, use:

```cpp
reg::source::FrameIdentityAccessUnitInjector injector;
std::vector<std::uint8_t> augmented;

const auto identity =
    injector.injectNext(
        encoded_access_unit,
        source_time_ns,
        augmented);
```

The injector preserves existing AUD/SPS/PPS/SEI prefix NAL ordering and inserts
the Reg SEI immediately before the first VCL NAL.

Do not insert the SEI after the following frame has already begun. Exact synchronization assumes the SEI belongs to the same source frame as the VCL NAL units that follow it.

### UNIGINE 2.22 RTSPStreamer

The supplied UNIGINE 2.22 documentation establishes a concrete post-encode
boundary:

`RTSPStreamer::addStreamFrameEncodedCallback()` receives each encoded H.264
frame as Annex-B bytes plus a presentation timestamp on the encoder thread.

The callback buffer is read-only and valid only during the callback. The public
API does not document a way to replace the bytes sent by RTSPStreamer's built-in
live555 server.

Therefore, with the documented public API, a Reg-compliant source must either:

- run RTSPStreamer with its built-in server disabled, inject FrameIdentity in
  the encoded callback, and republish the augmented Annex-B stream; or
- use a vendor/plugin implementation hook that inserts the SEI before the
  built-in live555 queue.

See `docs/UNIGINE_RTSPSTREAMER_INTEGRATION.md` for the exact API boundary and
integration topology.
