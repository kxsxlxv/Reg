# Source FrameIdentity validation

This probe validates the source-side H.264 FrameIdentity path after hardware
decode. It is intended for the UNIGINE -> RTP -> MediaMTX -> RTSP integration.

## Strict mode

Use:

```text
--require-frame-identity
```

Every decoded Vulkan frame must contain the canonical Reg
`AV_FRAME_DATA_SEI_UNREGISTERED` identity.

The probe also rejects a duplicate or regressing `frame_id` inside one
`stream_epoch`. Gaps are allowed because network/decode loss is not a
correspondence error.

A new `stream_epoch` is allowed after a source restart.

## Automatic finite probe

Use:

```text
--identity-probe-frames N
```

This implies `--require-frame-identity`. The application stops after at least
N decoded frames and prints one final result:

```text
[identity-probe] PASS decoded=300 identified=300 missing=0 non_monotonic=0 epoch_changes=0 target=300
```

A failing run exits with code 2.

## UNIGINE / MediaMTX smoke command

For the source path implemented in `kernel-sim`:

```bash
./reg_probe \
  --url rtsp://<render-machine-ip>:8555/reg \
  --identity-probe-frames 300 \
  --disable-overlay \
  --disable-recorder \
  --disable-telemetry
```

Expected periodic decoder output:

```text
[decoder] frame=1 size=1920x1080 epoch=<epoch> source_frame=<id>
[decoder] frame=120 size=1920x1080 epoch=<same-epoch> source_frame=<larger-id>
```

Do not test the original UNIGINE RTSPStreamer URL. The public encoded-frame
callback cannot replace the bytes already queued to its built-in live555
server, so that URL does not carry the injected Reg SEI.

## What PASS proves

A PASS proves, for the tested hardware/software path:

1. the source H.264 access unit contained the canonical Reg SEI;
2. RTP packetization and the MediaMTX relay preserved it;
3. FFmpeg RTSP demux/decode preserved the
   `user_data_unregistered` message;
4. NVIDIA Vulkan decode produced an `AVFrame` from which Reg recovered the
   exact `(stream_epoch, frame_id)`;
5. identities were monotonic within each observed epoch.

It does not yet prove Jetson inference/CVM1 correspondence. That is the next
hardware validation step after this probe passes.
