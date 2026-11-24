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
Today the legend provides illustrative ImGui OSD overlays on a user-supplied
photograph, **not parity-tested live rendering**. Mask/heatmap transport,
actual VTR binary message layout, and Jetson implementation are follow-ups.

For bbox: `edges`, `corners`, `diagonals` take
`none | solid | dashed | dash_dot`; `fill_alpha` is 0..1.
The label is optional: `label = { show = true, text = "MOT #{track_id}" }`.
Supported placeholders in preview:
`{track_id}`, `{id}`, `{class}`, `{class_id}`, `{conf_pct}`,
`{producer}`, `{origin}`.
A label prefers its specified outside edge, then the opposite edge, then
inside only if neither outside position fits. New catalogue defaults are
a solid box-colored background, no gap or rounding, and automatic black/
white text selected by relative luminance. Explicit legacy hex colors work.
`font_px`, `padding_px`, `gap_px`, `rounding_px` are honored in the preview.

Composable BBOX fields (all distances in displayed pixels):
- `width` remains the legacy fallback; `edge_width`, `corner_width`,
  `diagonal_width` independently override it.
- `corner_px`, `corner_layers` (1 or 2), `frame_layers` (1 or 2),
  `layer_gap_px` (inset for the second layer).
- `edge_markers` = none/center/outside; `marker_px`, `marker_width`.
- `rounding_px` controls the rounded perimeter; `outline_width` is
  additional dark contrast stroke on each side, not a second semantic frame.
- `fill` and `fill_alpha` control the fill independently of outlines.
The Primitives tab shows 15 neutral-color style combinations through the
same renderer used by Outputs. Samples are UI-only, not wire IDs.
The expanded syntax is additive on the Reg parser; senders must implement
it before consuming the new catalogue. Format and output IDs are unchanged.

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

## Authentic UAV photo preview (Reg phase 1)

The UI OSD demo renders a downsampled colour-indexed copy of the **user-provided**
image `1000031104.jpg` (source 1536×804), **not** an AI-generated picture,
a synthetic UAV, or a video frame. The sample photo is embedded in the
ImGui font atlas and is rendered by the same code in Vulkan and the SDL3
software screenshot test. No user-side image installation is necessary.

The reference manual bbox is `xyxy = [130, 55, 1111, 671]` in source pixels.
It includes the two wingtips, tail assembly and nose probe; labels and box
styles in `config/vtr.toml` are drawn dynamically over the original photo
through the existing `ImDrawList` primitives. Contour, sky and motion samples
are **illustrative only**, not pixel-accurate segmentation/GT or detector output.
The photo never modifies RTSP Raw frames, recordings, or CV algorithm inputs.

### BBOX renderer regression

The pure path builder in `VtrBoxGeometry.hpp` deduplicates vertices, closes solid
perimeters explicitly, and centers dashed/dash-dot patterns on opposite sides.
All contrast strokes are painted before colored strokes. The preview uses common
photo margins and a larger illustrative search ROI. `vtr_box_geometry` checks
17 style combinations at five sizes and two aspect ratios, including reflection
symmetry, non-degenerate segments and bounds. SDL snapshots remain visual smoke
tests; Vulkan pixel parity is not asserted.

Orthogonal BBOX paths now render as non-overlapping pixel-aligned filled
rectangles with square miter joins and butt caps. Requested widths round to
whole display pixels (minimum 1) for orthogonal strokes; curves and diagonals
retain fractional widths and geometric anti-aliasing without baked line textures.
The contrast underlay is opaque by default. The geometry test checks pixel-union coverage
for frame and L-corner widths 1, 2, 3, 4, 5 and 8. The Windows SDL raster test
checks actual output pixels for 1–4 px frames and corner coverage, and publishes
`vtr-bbox-raster.bmp`. It does not establish Vulkan raster parity.

### Configurable outline

`box.outline_color` accepts #RRGGBB, default #0C1014; `box.outline_alpha`
accepts 0..1, default 1. Both fields are optional for older catalogues.
`outline_width=0` or `outline_alpha=0` disables the underlay. The alpha is
applied once to the union of its components within one BBOX. Translucent
orthogonal strokes use a rectangle union; mixed paths use clipped 4x4 coverage
of stroke strips and joins. The opaque fast path is unchanged. Overlapping
separate BBOX objects remain separate layers. The SDL raster regression checks
color, alpha, overlap and disable behavior. Mixed translucent rendering cost
has not yet been benchmarked for live video; VTR is still preview-only.

### Interactive style editor

The Editor tab edits one draft at a time, with the same renderer as Outputs.
Photo/light/dark backgrounds, object zoom, 16/32/64 px examples and optional
composite preview are UI-only. The composite draws visible outputs in ascending
`z_order` (stable order by ID for equal values). Single-output previews remain
available even when hidden. `visible` defaults true and `z_order` defaults zero.
Live VTR receiving is not implemented: these metadata are exercised in previews.

BBOX `dash_px`, `gap_px`, `dot_px` use zero for pattern-specific legacy lengths.
`adaptive` defaults false for old files; the shipped styles enable it. It caps
stroke widths, corner/marker sizes, rounding and outline on small BBOX without
changing source geometry. `edge_alpha`, `corner_alpha`, `marker_alpha` default 1.
`corner_color` and `marker_color` accept hex or inherit. Component layers are
painted separately (edge/diagonal, corner, marker), so different translucent
components intentionally composite at intersections. Outline union is separate.
Label `max_width_px` caps width (zero auto) and truncates overly long text.

Save validates a temporary TOML and refuses stale hashes, preserves unknown keys
and output identity, backs up exact original bytes to `vtr.toml.bak` and replaces
the file. toml++ serialization can change formatting/comment placement or remove
comments; the backup retains them. Save only updates selected output values.
The Windows CI publishes `vtr-editor-preview.bmp` and tests parser/save round-trip.

### Factory config and portable package

The exact source catalogue is embedded at CMake configure time, so existing
installations can seed a missing config/vtr.toml after a binary-only update.
The portable ZIP includes config/vtr.default.toml (reference only), never the
mutable config/vtr.toml or its backups. First visit to VTR seeds a missing user
catalogue, leaving existing files (even malformed ones) untouched. Factory style
restores the selected built-in output into the draft, not the entire catalogue.

### Primitive compatibility checkpoint (before network integration)

Reg now previews the 10 non-BBOX primitive kinds alongside the original 15
BBOX examples. Vector OSD uses shared width, alpha, dash/dot/gap and outline
parameters; ellipse tessellation is capped at 96, preview paths at 4096 points.
Polygon fill is currently convex; raster mask and heatmap examples are still
illustrative, not actual image decoders. Arrow heads and point radius have
explicit size fields. These are Reg preview controls, not claims of live parity.

Read-only Jetson audit: kernel-vision master source/video/overlay.cpp and
source/video/overlay.hpp use NvDsDisplayMeta on a separate NVMM RGBA preview
branch; sky paint uses CUDA/EGL. The measured Nov 13 report
(docs/research/2026-11-13-semantic-osd.md in kernel-vision) reports 64 objects,
up to 12 line segments per patterned box, CPU OSD solid/dashed/corners and
about 60 FPS; GPU configuration currently validates solid-only styles.
No new Jetson code or network transport is introduced in this phase.
Before freezing transport, benchmark standalone primitive workloads on the
actual Jetson, recording CPU/GPU time, p95/p99, frame drops and memory copies.
Do not silently claim complex style parity based on Reg SDL previews.
