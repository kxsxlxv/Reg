# Reg diagnostics statistics core (Agent A)

Status: platform-independent C++23, synthetic-only offline tests. No live Reg/RTSP/Jetson integration and no HTTP service in this PR.

## Offline headless build

    cmake -S src/diagnostics/stats -B build/diag-stats -DCMAKE_BUILD_TYPE=Release
    cmake --build build/diag-stats --config Release
    ctest --test-dir build/diag-stats -C Release --output-on-failure

This standalone CMake project bypasses Vulkan, FFmpeg, SDL, GPU and network setup. Top-level CMake registers the same test when REG_BUILD_TESTS=ON. Verify the actual CMake/CTest results in CI; this documentation does not assert hardware validation. The headless harness requires only a C++23 compiler.

## Typed API

- MetricIdentity: required name, unit, owner, sample population, local monotonic clock domain, session identity, reset generation, kind and instrumentation support.
- DurationWindow: recordDuration(t_ns, duration_ns), recordSeconds(t_ns, seconds), recordOutcome(t_ns, outcome). Each successful duration is independent of Timeout, Missing, Skipped, Cancelled or Late counts. Reject NaN, Infinity, negative, oversized durations and negative/nonmonotonic observation timestamps. A missing measurement is not a zero duration.
- FramePacingWindow: recordFrame(t_ns) with actual measured stage boundary. Frame interval pairs must both lie within the query window. First frame has no interval. FPS equals (frame_count - 1) / elapsed seconds between retained first and last frames; max gap and quantiles reveal pacing issues hidden by average FPS.
- CounterRateWindow: observe(t_ns, uint64_counter, generation, explicitWrap=false). First sample has no rate; nonpositive elapsed time is rejected. A reset generation change or unmarked counter decrease clears previous samples, preventing invalid rates. An inferred decrease increments MetricIdentity::inferredResetSequence, so the new population cannot accidentally merge with its predecessor. A query that straddles a reset is marked incomplete. Only owner-declared unsigned wrap may bridge a decrease. Zero valid delta over positive elapsed time yields a genuine zero rate. Generation changes may restart the local clock.
- Snapshots support 1 s, 10 s, 60 s constants plus arbitrary inclusive [start_ns,end_ns] experiment windows; both endpoints and current query time share exactly one owner-local monotonic clock domain.

Availability states: Unsupported, Unavailable, Fresh, Stale. `availability` is evaluated using the last **in-window** observation at query time. `sourceAvailability` separately uses the latest observed stream timestamp at query time, even for an empty historical interval. Both `lastObservationNs` (in-window) and `lastSourceObservationNs` (stream-wide) are provided. An empty historical window with fresh newer samples is **Unavailable** at window scope, not Fresh. Absent samples have no mean, percentile or coverage; known failure-only populations have numeric zero coverage, not zero latency.

Every snapshot reports `historyTruncated`, `firstEvictedNs`, `lastEvictedNs` and `windowComplete`. History is incomplete when a known evicted sample intersects the inclusive window, or (for counters) the window straddles a reset boundary. Without an explicit partial-data opt-in, `meanNs`, `coverage`, `percentileUpper` and frame/counter FPS/rate refuse incomplete windows (optional absent); `allowPartial=true` on distribution methods permits clearly flagged descriptive values. `windowComplete` means **retained observations are complete according to the ring's known evictions**, not that the video source emitted every frame. A different field/signal must establish actual end-to-end source coverage.

Window bounds, query time, sample count, sum, exact min/max, per-outcome counts and metadata accompany snapshots. `rejectedInputs` is optional **stream-lifetime** metadata: available on a source snapshot and intentionally **unset after merge**, never naively summed. It cannot be reliably allocated to historical windows when an invalid timestamp is rejected; a future reset-scoped rejected-event stream could supply truly additive counts.

The owner and session/reset identity are mandatory for meaningful interpretation.

## Histogram and percentile accuracy

Integer logarithmic 1024-bin histogram; exact bins for 0-15 ns, then 16 subdivisions for each power-of-two range through UINT64_MAX. Quantile uses nearest rank max(1,ceil(P*n/100)), reported as the conservative bin upper endpoint: positive-value overestimate strictly below 6.25% (sampling error is not bounded). Every distribution snapshot carries `percentilesAreUpperBoundEstimates=true` and `quantileRelativeUpperErrorBound=0.0625`; HTTP/OpenAPI DTOs must expose the same method/accuracy metadata with the quantile and must never label it exact. No sorting on capture. At ~60 samples/s, P99 is nearly the maximum and is not a robust estimate of a true tail.

Two **separate** merge operations are provided:
- `mergedWith(other, Disjointness::VerifiedDisjoint)` merges same-window disjoint shards. Unverified by default, so `mergedWith(other)` **rejects**. The caller must establish disjoint underlying event identities externally; equal timestamps alone do not prove deduplication. Never pass verified evidence for overlapping event sets.
- `mergedAdjacentWith(later)` merges **ordered, directly adjacent inclusive nanosecond windows** (e.g. [0,9] + [10,19] → [0,19]). Time separation proves samples cannot overlap. Any gap, overlapping boundary, order reversal or different query time is rejected. This can form larger window distributions from nonoverlapping adjacent buckets.

Both operations reject incompatible names, kind, unit, owner, population, clock domain, session, reset generation, support and (by default) incomplete windows. Both check uint64 histogram/count overflow and recompute tails from summed bins rather than averaging P95. `allowPartial=true` explicitly propagates incomplete status; percentile/mean remain absent unless queried with a separate partial opt-in. Across-host clock monotonic timestamps may **never** be subtracted; distinct clock domains are incompatible. `rejectedInputs` is deliberately null after any merge.

## Memory, work, concurrency

Series are single-writer and have preallocated fixed-size rings (default capacity 16,384). Capture: O(1), no dynamic allocation or file/network I/O. Read-side snapshots: O(retained observations + 1024) and allocate histogram/metadata copies; do not invoke snapshots or serialize them on decode/render/CV callbacks. memoryBoundBytes() returns the ring capacity in bytes: typically 24 bytes per duration record, 24 per counter reading, 8 per frame timestamp. A capacity of 16,384 covers 60 seconds at 120 observations/s under nominal conditions; bursts can evict earlier observations, so the requested window may be incomplete. No automatic completeness claim is made. Synchronize externally or use a bounded queue between capture and snapshot threads.

## Synthetic examples

| Scenario | Samples | Result |
| --- | --- | --- |
| 60 FPS steady / one second | 61 frames, 60 gaps | measured FPS 60, max gap about 16.67 ms |
| 120 FPS steady / one second | 121 frames, 120 gaps | measured FPS 120, max gap about 8.33 ms |
| 60 FPS bursty | 61 frames, 60 gaps | mean 60 FPS, 180 ms max gap, higher P95/P99 |
| No measurements | 0 successes, 0 outcomes | mean/quantiles/coverage absent |
| 10 successes, 4 failures | 14 outcomes | coverage 10/14; failures excluded from successful latency percentiles |
| Decoder reset | first new-generation counter sample | rate unavailable; reset event increments |
| Ring overflow, 4 observations in 3 slots | historical [1,4] | `historyTruncated=true`, `windowComplete=false`, percentile/FPS/rate unavailable |
| Window wholly after eviction | retained [2,4] | `windowComplete=true`; distributions available |
| Newer source samples outside historical query | [0,20] with source still recording at 990 | source Fresh, window Stale / Unavailable |
| Unverified same-window histogram merge | two same-key distributions | rejected without explicit disjointness evidence |
| Nonadjacent or overlapping windows | [0,9] plus [9,19] / [11,19] | rejected; no invented aggregate |

## Integration boundaries (no shared-contract changes)

1. Coordinator: sample the existing authoritative TelemetryModel::Counters on a separate stats owner thread, one CounterRateWindow per stage/metric and decoder process/session generation. Present Raw/demux/decoded/Overlay rates separately. Do not replace working UI or critical-path instrumentation in this PR.
2. Agent C / coordinator: translate exact eligible-frame lifecycle (stream_epoch, frame_id) into mutually exclusive successful/late/missing/timeout/skipped observations. An existing matched_only latency stream is not an all-eligible CV delay distribution. Sending CV metadata successfully does not prove it arrived.
3. Agent B: keep independent diag-fixture/v1 JSON observation schema and replay; adapter later maps scalar owner-local observations into these typed APIs without moving statistics into Python or an LLM.
4. Coordinator: export immutable typed snapshots through future read-only HTTP DTOs. Numeric uint64 identities/counters must be decimal strings in JSON. Report availability, sample_count, population, window and reset scope. No GPU/Jetson validation is implied.

Open integration decisions: canonical metric names and model-population labels, session-to-generation mapping, and the Exact Sync eligible-frame denominator.
