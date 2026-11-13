# GT distance + CV detection timeline (Overview)

## Purpose

REG renders a view-only live timeline of GT distances from MainPlayer to each
active GT object. Jetson **does not receive GT**. REG compares received CVM1
predictions against source-synchronized GT bounding boxes for presentation.

This visualizer is **not a ground-truth evaluator**: independent, device-local
logs remain authoritative for metrics. Dropped RTSP GT SEI (often 10-20%) must
not change inference statistics or count as a miss. UI joins are best-effort.

## UI

- The Overview tab keeps the existing performance panel.
- Below it, a single column of full-width GT charts occupies all free height.
  There is no vertical scrollbar. When there are more active objects than fit,
  use the horizontal page selectors. Stable ordering is by GT object ID.
- Left-click within a chart to focus it at full available height; click again
  to return to all charts.
- **Shift+Space** freezes the *display* at the current snapshot, not the
  decoder, CVM1 receiver or telemetry recorder. Press again to return to live.
- Hover over real GT points or matched CV markers for exact frame ID, source
  timestamp, measured GT range, CV track ID, confidence and IoU.
- CV markers are colored according to existing CVM1 flags. These labels are
  not inferred algorithm phases or claims of successful confirmation.
- Events have their own **События** tab. "Текущие цели" is no longer shown.

## Data path

- A valid ready KGT1 SEI can contain optional finite non-negative \`range_m\`.
  Missing/null/negative/non-finite values are *not zero observations*.
- Only valid source-frame GT (already checked against RGF1) is accepted.
  The GT plotting stream is capped at about **20 observations/s/object**;
  exact-frame CV markers retain their original (not sampled) GT range.
- CVM1 is observed from the accepted metadata callback, independent of overlay
  display success and of overlay presentation deadlines.
- Two independently arriving messages are paired **only** when their
  \`(stream_epoch, frame_id)\` agrees. GT bbox is normalized using the GT video
  dimensions, then compared with CVM1 normalized bounding boxes.
- Eligible pair scores are bbox IoU. Greedy highest-IoU, one-to-one matches
  with a default threshold of **0.30** prevent duplicating a CV hit across
  several GT objects. This is visual association, not ID ground truth.
- On GT absence, no point is created. The chart connects close valid points
  across short gaps, and deliberately disconnects long gaps (>400ms by
  default); it does not extrapolate into future time or generate zeros.
- Pending GT/CV frames are bounded and expire after 3s. For safe bounded
  visualization history, data older than 5min is pruned.
- The RTSP stream reconnect resets history: reused GT IDs may refer to
  different physical objects in a new source epoch.

## Configuration without recompiling

Edit \`config/gt-range-ui.json\` next to the \`reg_probe\` executable.
The chart polls the file modification timestamp every ~300ms and applies
changes on the next UI update; when JSON is invalid it continues with
the previous valid appearance. It also recognizes that path relative
to the current working directory for developer runs.

Fields:

- \`window_seconds\`: visible history window, clamped to 5–300 seconds.
- \`gap_ms\`: maximum time between GT samples drawn with a solid line.
- \`range_color\`: \`#RRGGBB\` for the measured distance curve.
- \`cv_markers\`: ordered rule list with \`label\`, \`flags_mask\`,
  \`flags_value\`, \`color\`, and \`marker_size\`. Nonzero masks are checked
  first; a mask of zero is the default style for all other CV predictions.
  Existing 0x0001 detector and 0x0002 propagated flags work immediately.

Changing visualization labels/colors/rules requires **no recompile**.
A future Jetson protocol with new fields will need a small typed adapter to
translate those fields into semantic events; a config file cannot automatically
decode an unspecified transport schema. No new tracking stages are invented
by the current implementation.

## Acceptance tests

\`ctest -R "gt_sei_osd|gt_range_history|telemetry_model"\`

Required cases: missing/null/negative \`range_m\`, real zero distance, random
missing GT frames, exact FrameKey join in both arrival orders, cross-epoch
rejection, bad IoU rejection, stream reset, and no invented zero samples.

## Limitations

- UI charts are sampled for performance; the device-local records remain the
  only source for statistically precise scoring.
- A live chart can only associate CV with a GT frame present and validated in
  REG's incoming RTSP stream.
- No phase/confirmation/neural tracking model is inferred yet.
- A single view cannot fit arbitrarily many objects legibly; paging is used
  rather than vertical scrolling.
- The current hotkey acts while a REG SDL window has keyboard focus (not as an
  operating-system global keyboard hook).
