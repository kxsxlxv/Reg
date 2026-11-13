# CVM1 v1 target flags — Reg compatibility contract

The CVM1 v1 **binary layout is unchanged** (16-bit TargetMetadata.flags).

| Bit | Semantic dimension | Meaning |
|---|---|---|
| 0x0001 | provenance | detector observation |
| 0x0002 | geometry | propagated geometry |
| 0x0004 | lifecycle | tentative |
| 0x0008 | lifecycle | confirmed |
| 0x0010 | selection | selected SOT |
| 0x0020 | quality | weak |
| 0x0040 | lifecycle | lost |
| 0x0080 | geometry | predicted |
| 0x0100 | geometry | held |
| 0xFE00 | unallocated | ignored by current resolver |

Priority: selected SOT (weak -> sot_weak), tentative, predicted,
propagated/held, confirmed, detection, fallback. The combined confirmed +
predicted state is therefore *predicted*. Conflicting lifecycle states
(tentative/confirmed/lost), predicted+held, or selected+lost are neutral
fallback regardless of configurable rules. Other dimensions may coexist.

Semantic appearance is shared by the Overlay video and GT-range charts via
`config/gt-range-ui.json` (hot reload approximately 300ms).
`styles` configures color, width, pattern, marker_size, label and visibility.
Ordered optional `rules` (flags_mask/flags_value/style) override normal
precedence for known, nonconflicting bit combinations. Legacy `cv_markers`
rules remain a compatible visual fallback when `styles` is absent.
No algorithm/model names are hardwired into the resolver.

**Frame-local IDs — provisional, requires explicit Jetson agreement.**
CVM1 requires uint64 id. Proposed encoding: reserve the top bit as
frame-local-only (`0x8000000000000000 | per_frame_index`). Persistent
track IDs must not set that bit. REG does not join these IDs into a
trajectory across frames; the pair (FrameKey, per_frame_index) identifies
only one observation. This encoding is NOT claimed ratified by the Jetson
agent because the provided `25bccf0` source and authoritative file were
not accessible in this environment.

Only exact (stream_epoch,frame_id) matching between CVM1 and validated KGT1
may produce GT-range markers. Lack of SEI or drops must not be silently
interpreted as a miss or inferred by arrival timestamps. The independent
device-local logs remain authoritative for all evaluation metrics.
