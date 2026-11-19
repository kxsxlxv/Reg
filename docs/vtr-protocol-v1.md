# Vision To Reg (VTR) — minimal catalogue and OSD proposal

Status: **Reg phase 1 (TOML parser + offline OSD legend)**. This is **not**
a live VTR UDP receiver and does not replace existing CVM1.

## The one-file contract

- `config/vtr.toml` is the authoritative **output catalogue**, not a list
  of algorithms or a graph of processing stages.
- Each `[output.name]` independently defines a numeric `id`, `semantic`,
  `geometry`, optional `benchmark`, and OSD style `box`/`osd`/`label`.
- Two sources (neural/Canny) may produce the same semantic `detection`.
  The supervisor can route their boxes to MOT/SOT; VTR doesn't decide routing.
- `format = 1` selects how to parse TOML. It is **not** a manual revision.
  FNV-1a 64 of the exact file bytes selects the **specific dictionary**.
  This check detects accidental mismatch, not tampering.
- `benchmark = "none"` is a declaration of intent, not an implementation
  of all listed GT metrics; Reg has not yet connected VTR outputs to GT joins.
- Only the locally installed file is read. Copy from Jetson manually or
  with SCP; press **Обновить TOML** in the telemetry VTR tab. If parsing fails,
  the last valid catalogue stays in memory and an error appears.

## Target portable OSD primitive set

`bbox` (rectangle with separately styled edges/corners/diagonals/fill),
`line`, `polyline`, `polygon`, `circle`, `ellipse`, `point`,
`arrow`, `text` (label), `mask`, `heatmap`.

The enum and style grammar must be implemented identically on Jetson and Reg.
Today the legend provides a schematic ImGui preview of these declared
geometry classes, **not parity-tested live rendering**. Mask/heatmap transport,
actual VTR binary message layout, and Jetson implementation are follow-ups.

For bbox: `edges`, `corners`, `diagonals` take
`none | solid | dashed`; `fill_alpha` is 0..1.
The label is optional: `label = { show = true, text = "MOT #{track_id}" }`.
Supported placeholders in preview:
`{track_id}`, `{id}`, `{class}`, `{class_id}`, `{conf_pct}`,
`{producer}`, `{origin}`.
A label is anchored outside the bbox (top-left or bottom-left) with a
semi-transparent background.

## Constraints before sending binary VTR

1. Use a stable little-endian fixed **envelope** with magic/format,
   session ID, dictionary hash, FrameKey `(stream_epoch, frame_id)`,
   and bounded payload length.
2. No VTR packet is decoded using a dictionary with a mismatching hash.
   Changing a file's whitespace also changes the hash by design.
3. Use fixed geometry encodings; the catalogue does **not** define arbitrary
   binary structs. Payloads and masks must be size-limited, validated.
4. Keep GT metrics and source provenance distinct from visual primitives:
   ROI and operator-only fill are not benchmark detections.
5. Keep CVM1 operational until the sender and receiver implement and
   verify the new VTR data path independently. No UI widgets or scripts.
