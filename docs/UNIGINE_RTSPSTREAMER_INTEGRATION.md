# UNIGINE 2.22 RTSPStreamer FrameIdentity integration

This document records the source-side integration boundary established from the
UNIGINE 2.22 RTSPStreamer documentation supplied for Reg.

Relevant UNIGINE documentation pages:

- `api/library/plugins/rtspstreamer/class.rtspstreamer_cpp.md`
- `code/plugins/rtspstreamer/index_cpp.md`

## What the public API exposes

UNIGINE RTSPStreamer renders a `Player` camera, converts RGBA to NV12 on the
GPU, and encodes H.264 with NVIDIA NVENC.

The public C++ API exposes:

```cpp
RTSPStreamer::FrameCallbackHandle
RTSPStreamer::addStreamFrameEncodedCallback(
    const RTSPStreamer::StreamHandle &stream,
    CallbackBase3<
        const char *,
        unsigned int,
        unsigned long long> *callback);
```

The callback receives:

1. encoded H.264 frame bytes;
2. encoded byte count;
3. frame presentation timestamp.

The H.264 data is documented as **Annex-B**.

The callback runs on the **encoder thread**. The data pointer is valid only for
the duration of the callback and therefore must be copied before it is retained
asynchronously.

UNIGINE also documents that encoded-frame callbacks remain available when the
built-in RTSP server is disabled with:

```json
{
    "start_server": false,
    "port": 8554,
    "transport": "any"
}
```

## Important public-API limitation

The encoded-frame callback receives a read-only `const char *`.

The documented public API does not expose a method to:

- replace the encoded frame that will be sent by the built-in live555 server;
- inject an additional H.264 NAL into the built-in server queue;
- mutate the callback buffer in place.

Therefore the built-in RTSP server cannot be assumed to carry Reg FrameIdentity
SEI merely because an encoded callback is registered.

Using the built-in server unchanged while adding SEI only in the callback would
create two different bitstreams:

```text
built-in RTSP clients      <- original H.264 without Reg SEI
encoded-frame callback     <- application can construct augmented H.264
```

That topology does **not** satisfy Reg exact-frame synchronization.

## Supported Reg topology with the public API

The public API supports a correct source path when RTSPStreamer is used in
callback-only mode and the callback output is republished after FrameIdentity
injection:

```text
UNIGINE Player
    -> RTSPStreamer GPU RGBA -> NV12
    -> NVENC H.264
    -> addStreamFrameEncodedCallback()
    -> Reg Annex-B FrameIdentity injector
    -> bounded non-blocking encoded-AU queue
    -> RTSP publisher
       -> Viewer
       -> Jetson
```

Both consumers must receive the **same augmented bitstream**.

An alternative is a vendor/plugin-source modification that inserts the Reg SEI
inside RTSPStreamer before its live555 queue. That requires implementation
access beyond the documented public API.

## Reg access-unit injector

Reg provides:

```cpp
#include "source/FrameIdentityAccessUnitInjector.hpp"

reg::source::FrameIdentityAccessUnitInjector injector;
std::vector<std::uint8_t> augmented;

const auto identity =
    injector.injectNext(
        encoded_annex_b_access_unit,
        source_time_ns,
        augmented);
```

The injector:

- generates/uses one `stream_epoch` for a logical encoder session;
- assigns monotonically increasing `frame_id`;
- constructs the canonical `user_data_unregistered` SEI;
- parses the Annex-B access unit;
- preserves existing prefix NAL ordering;
- inserts the Reg SEI immediately before the first H.264 VCL NAL;
- never matches frames using timestamps.

For an access unit:

```text
AUD | SPS | PPS | IDR
```

the result is:

```text
AUD | SPS | PPS | Reg SEI | IDR
```

This is preferable to blindly prepending the SEI because an Access Unit
Delimiter, when present, remains the first NAL in the access unit.

## UNIGINE callback binding

UNIGINE callbacks can be created with `MakeCallback()`. A source application
can bind the encoded-frame callback as follows:

```cpp
#include <UnigineCallback.h>
#include <plugins/Unigine/RTSPStreamer/UnigineRTSPStreamer.h>

#include "source/FrameIdentityAccessUnitInjector.hpp"

using namespace Unigine;
using namespace Unigine::Plugins;

class RegRtspFrameBridge
{
public:
    bool attach(
        RTSPStreamer *streamer,
        RTSPStreamer::StreamHandle stream)
    {
        streamer_ = streamer;
        stream_ = stream;

        callback_ =
            streamer_->addStreamFrameEncodedCallback(
                stream_,
                MakeCallback(
                    this,
                    &RegRtspFrameBridge::on_encoded_frame));

        return callback_.value != 0;
    }

    void detach()
    {
        if (streamer_ && callback_.value != 0)
            streamer_->removeStreamFrameEncodedCallback(
                stream_,
                callback_);

        callback_ = {};
        streamer_ = nullptr;
    }

private:
    void on_encoded_frame(
        const char *data,
        unsigned int size,
        unsigned long long presentation_timestamp)
    {
        const auto bytes =
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t *>(data),
                static_cast<std::size_t>(size)};

        const std::uint64_t source_time_ns =
            source_monotonic_time_ns();

        const auto identity =
            injector_.injectNext(
                bytes,
                source_time_ns,
                augmented_);

        // Required: this operation must only copy/move into a bounded queue.
        // It must not perform blocking socket/server I/O on the UNIGINE
        // encoder thread.
        publisher_queue_.try_submit(
            augmented_,
            presentation_timestamp,
            identity);
    }

    RTSPStreamer *streamer_{};
    RTSPStreamer::StreamHandle stream_{};
    RTSPStreamer::FrameCallbackHandle callback_{};

    reg::source::FrameIdentityAccessUnitInjector injector_;
    std::vector<std::uint8_t> augmented_;

    // Application-owned bounded queue; concrete publisher implementation is
    // intentionally omitted here.
    PublisherQueue publisher_queue_;
};
```

The snippet shows the required ownership and callback boundary. The concrete
`PublisherQueue` and RTSP publisher are deployment-specific and must not block
the encoder callback.

## Timestamp rule

The third RTSPStreamer callback argument is documented as the frame
presentation timestamp, but the supplied documentation does not state its
timebase/unit.

Do **not** copy that integer into `source_time_ns` unless its unit is verified.

`source_time_ns` is diagnostic only. The exact correspondence key remains:

```text
(stream_epoch, frame_id)
```

For the first integration pass, a source-host monotonic nanosecond timestamp
captured at the callback boundary is acceptable for diagnostics. A later
render-stage timestamp can replace it without changing the matching contract.

The RTSPStreamer presentation timestamp should still be preserved separately
for the downstream RTSP publisher's media timing.

## Encoder settings for the Reg source

For the low-latency Reg deployment, use the RTSPStreamer settings consistent
with the MVP architecture:

```cpp
RTSPStreamer::StreamDescription desc;
desc.width = 1920;
desc.height = 1080;
desc.target_fps = 60.0f;
desc.bitrate_kbps = 0;
desc.slot_count = 1;
desc.encoder.preset =
    RTSPStreamer::EncoderPreset::UltraLowLatency;
```

The UNIGINE documentation states that its live-streaming H.264 configuration
disables B-frames. That is compatible with the current Reg MVP assumptions.

## Realtime rules

The encoded callback executes on UNIGINE's encoder worker thread.

Therefore:

- do not perform blocking RTSP/socket I/O in the callback;
- do not wait for disk;
- do not wait for Jetson or Viewer acknowledgements;
- copy/transform the compressed access unit and enqueue it into a bounded
  non-blocking publisher queue;
- if the publisher queue is full, report/drop according to source telemetry
  rather than allowing unbounded latency growth.

This compressed-bitstream copy does not violate Reg's decoded-frame zero-copy
invariant. The forbidden path is decoded GPU pixels travelling
GPU -> CPU -> GPU during normal Viewer operation.

## Acceptance test for this source boundary

Before declaring source FrameIdentity integration complete:

1. Run RTSPStreamer in callback-only mode.
2. Feed every callback frame through
   `FrameIdentityAccessUnitInjector`.
3. Publish the resulting Annex-B access units through the selected RTSP
   publisher.
4. Connect both Viewer and Jetson to that published stream.
5. Verify every decoded frame exposes the Reg
   `AV_FRAME_DATA_SEI_UNREGISTERED` payload.
6. Verify `stream_epoch` changes after source-session restart.
7. Verify `frame_id` is monotonic within an epoch.
8. Verify the Viewer and Jetson observe the same exact FrameKey for the same
   visual frame.
9. Exercise frame drops and variable Jetson inference latency; exact matching
   must remain unchanged.

## Remaining implementation decision

A concrete RTSP republisher is still required when using only the documented
public RTSPStreamer API.

The two valid implementation choices are:

1. callback-only RTSPStreamer plus an application/sidecar RTSP publisher fed by
   the augmented Annex-B access units;
2. modify/extend RTSPStreamer itself, if UNIGINE provides plugin source or an
   additional encoded-frame mutation/NAL-injection API.

Do not route Viewer/Jetson to the built-in unmodified RTSP URL and assume the
callback-injected SEI is present there.
