# FastVision CVV1 in Reg (opt-in)

This feature is intentionally isolated from the existing CVM1 protocol.

## Launch / compatibility

By default, **CVV1 is disabled**, so the original CVM1 UDP/50010
receive, store, exact-frame drop policy, telemetry, and renderer stay
unchanged. The Launcher exposes **FastVision CVV1** in the network card,
with UDP port (50011 by default) and separate YOLO / Candidates / Target /
Status switches. Existing schema-v1 profiles load with CVV1 disabled.

Equivalent CLI:

```text
reg_probe --url rtsp://192.168.50.1:8555/reg --enable-cvv1 --cvv1-port 50011
```

Individual controls: `--cvv1-hide-yolo`, `--cvv1-hide-candidates`,
`--cvv1-hide-target`, `--cvv1-hide-status`.

The CVM1 port stays 50010. CVV1 is a second socket and must use a
different UDP port. Firewalls must permit incoming UDP/50011.

## Receiver and semantics

- IPv4 sender endpoint + sender session ID + snapshot ID reassembly;
  little-endian 48-byte envelope, CRC32C (shared with CVM1), <=1400
  bytes/datagram, exactly 1348-byte fixed chunk geometry.
- Bounded first-fragment timeout (1 second), 64 in-progress records,
  16 MiB of actual chunk payloads, and 1024 completed/rejected IDs.
- JSON parses only after full assembly, on the CVV1 receive thread.
  Rejects malformed IDs, nonfinite numbers, invalid bounding boxes,
  inconsistent count/truncation markers, and nesting >16.
- All records are replacement state for one RGF1 FrameKey. No interpolation
  or nearest-timestamp fallback. Sender monotonic nanoseconds never get
  subtracted from Windows clocks.
- When CVV1 is active, video is allowed to present even if CVM1 has not
  arrived. When an exact CVV1 snapshot exists, its four layers are
  authoritative. A null selected target suppresses CVM1 target rendering.
  When none exists, the legacy exact CVM1 overlay is the fallback.
- The snapshot remains cached across Vulkan render retries and is evicted
  only after successful presentation. A later/older snapshot cannot update
  an already-presented frame.
- Video loss and decoder reconnect clear all relevant frame caches.
- Text labels use persistent candidate IDs and separate session target IDs.
  Missed candidates have faded/dashed last-seen boxes; they are *not*
  predictions and are not treated as fresh detections.
- CVM1 packet interpretation, recording, and target-control contract
  are deliberately untouched. CVV1 contains diagnostics only.

## Offline tests

```text
ctest --test-dir build -R cvv1_offline --output-on-failure
```

`tests/cvv1_tests.cpp` exercises JSON schema/uint64 precision, invalid
coordinates, confirmation/grace, explicit reset, layered rendering,
multi-datagram reverse ordering, CRC failures, conflicting duplicates,
timeout, endpoint/session separation, and the exact FrameKey cache.

The authoritative eight JSON/hex sender fixtures and the independent
Python oracle are distributed in the Jetson handoff archive
`reg-cvv1-handoff.zip`. For a sender-to-receiver verification before
merging, run the sender's reference suite separately:

```bash
python3 -m unittest tools.cvv1.test_decoder -v
```

Hardware smoke test: send CVV1/50011 and CVM1/50010 while streaming RTSP.
Verify SEARCH -> CONFIRMATION -> TRACK -> RESET with the selected target
removed on reset, monitor-only empty YOLO, stale overlays, and legacy
CVM1-only mode after unchecking CVV1.
