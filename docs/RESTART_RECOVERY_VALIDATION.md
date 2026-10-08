# Reg restart / recovery acceptance

## Why the old build failed

Two independent runtime behaviors explained the operator's four scenarios:

1. The normal Raw video path passed only the source-frame diagnostic
   `ImGuiOverlayRenderer` to `VideoRenderer::render(..., overlay)`. The
   two-argument video overload was NetImgui-aware, but the Raw call did not
   use it. NetImgui was visible only while `renderBlank()` was used for
   NO SIGNAL. Raw video and the remote UI were never composited together.
2. The RTSP reconnection loop ran only when `RtspDecoder::run()` returned
   or threw. If UDP input went silent after MediaMTX stopped, the FFmpeg
   `av_read_frame()` loop could keep blocking or returning EAGAIN. The
   1500-ms UI "NO SIGNAL" timeout did **not** terminate that decoder session.
   The reconnect loop could therefore remain idle indefinitely.

## Implementation

- Reg's Raw renderer is the only NetImgui overlay owner. Live Raw video is
  drawn first, the local RGF1/SEI overlay second, and NetImgui last within
  the same Vulkan rendering pass, using the existing
  `OverlayChainRecorder`. The CV renderer cannot claim the NetImgui TCP
  listener if the Raw window is temporarily unavailable.
- RTSP/UDP open and stream-info operations use 12-second monotonic FFmpeg
  interrupt deadlines. A decoded/live session uses a 5-second
  no-demuxed-packet deadline. EAGAIN does not renew that deadline; only
  successfully received packets renew it. Timeout raises an ordinary
  decoder error so the existing safe Vulkan-session retirement and bounded
  reconnect backoff run. Local file replay has no such network deadlines.
- The UI's faster 1500-ms NO SIGNAL transition stays independent of the
  5-second network reconnect timeout. No source/metadata protocol changes.
- Regression test `reconnect_render_contract` covers overlay ordering and
  monotonic deadline behavior without requiring hardware. Full runtime
  recovery must also be verified on Windows and the live RTSP sender.

## Manual 8-case matrix

Use the existing Launcher Development profile with NetImgui enabled and
RTSP `rtsp://192.168.50.1:8555/reg`. Keep the UNIGINE/MediaMTX source
unchanged. Reg child logs are under
`data/sessions/<timestamp>/stdout.log` and `stderr.log`.

| Case | Action | Acceptance |
| --- | --- | --- |
| A | Start MediaMTX → UNIGINE → Launcher → Reg | Raw RTSP video and NetImgui both visible **simultaneously**. |
| B | Restart Reg while the sender remains active | New decoded frames and NetImgui remain visible simultaneously. |
| C | Restart UNIGINE while Reg is running | NetImgui stays visible during NO SIGNAL; video resumes automatically and NetImgui stays visible. |
| D | Restart MediaMTX while Reg and UNIGINE are running | NO SIGNAL appears promptly, FFmpeg session times out/reopens, stream resumes automatically without restarting Reg; NetImgui persists. |
| E | Start UNIGINE before MediaMTX, then Reg | Source recovery and Reg attachment work without a prescribed launch order. |
| F | Start Reg before MediaMTX or UNIGINE | NetImgui appears while waiting; later video arrival does not hide it. |
| G | Restart MediaMTX then UNIGINE while Reg keeps running | Reconnect/epoch transition works; no permanent frozen image or stale CV metadata. |
| H | Disconnect and reconnect the NetImgui producer while RTSP is playing | Video stays live; the remote UI returns without restarting Reg. |

Expected logs after a transport outage include an explicit
`RTSP read stalled: no demuxed packets for 5000 ms` (if the
silent-read timeout triggers), a `[watchdog] reconnect=` line,
and then `[watchdog] RTSP session opened` once the publisher is back.
If FFmpeg notices the failure earlier, the error may instead be
`av_read_frame` or RTSP connection error.

These are regression acceptance expectations, not claims that all eight
physical-device cases have been run. Exercise multiple restart orders and
repeated cycles. Record RTSP session count, reconnect count, received
packet/frame rate, and NetImgui TCP connection status. A UDP socket send
success in kernel-sim alone is not sufficient evidence of client recovery.

## Open runtime considerations

- FFmpeg interrupt callbacks are needed to abort a blocking UDP read.
  The existing library supports interrupt callbacks; physical
  MediaMTX/Windows validation is still necessary.
- A raw-video renderer/swapchain surface-loss recovery test is separate
  from restarting MediaMTX; both must leave NetImgui usable.
- The exact CV overlay is independently gated by CVM1 availability.
  During Jetson outages, absence of CV annotations is expected and
  unrelated to Raw NetImgui composition.
