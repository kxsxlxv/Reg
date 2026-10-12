# Reg diagnostics statistics core (Agent A)

**Implementation status:** isolated offline C++23 library and autonomous synthetic tests. No runtime producer, HTTP API, JSON serializer, Jetson or RTSP integration is implied.

## Build without any Reg video/GUI dependencies

```sh
cmake -S src/diagnostics/stats -B build/diag-core -DCMAKE_BUILD_TYPE=Release
cmake --build build/diag-core --parallel
ctest --test-dir build/diag-core --output-on-failure
```

In a full Reg configuration, root `CMakeLists.txt` adds the same `reg_diagnostics_core` library and registers `diagnostic_statistics` when `REG_BUILD_TESTS=ON`. Core builds do not link Vulkan, FFmpeg, SDL, ImGui, RTSP, or any network library. The full Reg configuration still independently requires its existing external dependencies.

## Typed contract

`MetricIdentity` must specify **name, kind, unit, owner, sample_population, local clock_domain, process session_id and reset_id**; none may be empty. A series is permanently bound to that identity. On decoder/process/source reset create a new series with a different reset/session identity. Do **not** append one session to another. The API does not accept an arbitrary per-observation host clock or FrameKey and therefore cannot accidentally subtract two hosts' timestamps or treat frame_id alone as identity.

`DurationSeries::record(local_ns, Outcome::Success, duration_ns)` accepts successful *measured* latencies; `Missing`, `Timeout`, `Skipped` accept no numeric duration. A failed call rejects the input, increments `quality.rejected_inputs`, and cannot inject artificial zero values. For double-backed adapters, `record_double` rejects NaN, infinity, negative, fractional ns and values outside uint64. Native uint64 samples retain their exact order statistics. `Timeout` and `Missing` are included in the eligible denominator, while intentionally `Skipped` is not. `success_coverage` is missing/null when `eligible==0`. Coverage describes **the caller's declared population**, not full lifecycle coverage unless the caller accounts for all eligible frames. In particular, P95 of successful Exact Sync matches remains matched-only.

`CounterSeries::record(local_ns, uint64_value)` captures monotonic raw counters. A decrease (including apparent uint64 wrap) is an **inferred reset**, not a huge wrapped delta. A query whose window includes a reset marker reports `ResetBoundary` with **no numeric rate**; subsequent windows after that marker can regain a valid rate from two or more new samples. Equal counter values at different times give a genuine zero rate; one sample gives no rate. Delta is calculated using the first/last samples *inside* the requested window (`delta / elapsed_ns`), not an average of instantaneous FPS estimates; it never extrapolates beyond measured counter endpoints.

`FrameIntervalSeries::record_frame(local_ns)` records successful arrivals at one pipeline boundary. Every interval belongs to its **ending** frame. Its first frame produces no interval. It reports completed-gap distribution, longest completed inter-frame gap, and the ongoing age since the last frame (including when the window has no new frames); it does not infer packet loss or missing FrameKeys from a long gap. Use a distinct metric identity for source frames, decoded frames, Raw presents, and CV-matched overlays.

All timestamps are signed nonnegative 64-bit **owner-local monotonic nanoseconds**, injected by callers (or a deterministic virtual clock in tests). Each accepted sample must have a strictly increasing timestamp. Repeated/backward/negative time is rejected; time windows are half-open `[begin_ns, end_ns)` and must end by the injected `now_ns + 1`. `TimeWindow::last_seconds(now, 1/10/60)` provides standard named windows with the observation at `now` included; arbitrary experiment windows use `TimeWindow{begin,end}`. We do not infer transit delay by subtracting Windows/Jetson/Ubuntu monotonic clocks.

`Quality.availability`: `NeverObserved` (startup), `Fresh`, `Stale` (current time exceeds last accepted observation by configured threshold), `Disabled`, `Unsupported`, `Error`. `set_availability` allows a future adapter to explicitly signal the latter states. A zero sample count is not a substitute for availability; empty distributions have no min, max, mean or quantiles. Historical distributions can still exist in a stale snapshot but must **never** be presented as a fresh live measurement. `quality.history_truncated` means samples belonging to the requested window were evicted by fixed capacity and statistical answers are **partial**. No confidence or completeness claim is permitted for a truncated result.

## Quantiles, memory and computation

For one retained stream/window, P50/P95/P99 use exact **nearest rank** `sorted[ceil(p * n) - 1]`, not interpolated quantiles. Example: 4 success samples `[10,20,30,40]` give P50=20, P95=40, P99=40. With `n < 100`, P99 can simply equal the maximum; surface `sample_count` and don't imply tail confidence. Quantile error is **zero versus the retained sample population**, but the estimate may be statistically uncertain or incomplete due to fixed history eviction. Complexity:

| Operation | Complexity | Allocation |
| --- | --- | --- |
| Capture counter/duration/frame | O(1) | None after construction |
| Fixed-memory ring storage | O(capacity) | One vector at construction |
| Snapshot count/sum/gap selection | O(retained samples) | One bounded temporary vector for duration/gaps |
| Snapshot exact quantiles | O(k log k), k <= capacity | Only on observer/query thread |
| Merge sufficient stats | O(1) | Identity/string copying for output |

Default capacity is 8,192 samples per series. At 120 observations/second, that holds approximately 68 seconds, enough for a complete 60-second statistic **if there are no additional observations/measurement categories stored in that particular ring**. More than 120/sec or a burst can evict samples before 60 seconds: explicitly monitor `history_truncated` and configure a larger capacity for the workload. The ring's sample buffer is fixed: on the measured Linux/GCC build, 4,096 `DurationSeries` slots used **98,304 bytes**; a `FrameIntervalSeries` or `CounterSeries` may have different `sizeof`/padding on Windows. `reserved_bytes()` exposes sample-slot memory, excluding identity strings, vector metadata and temporary snapshot work.

`Distribution::sum_ns` is a long double sufficient statistic; Windows/MSVC may implement long double as double, so **very large sums can round** even though exact uint64 min/max/quantiles remain exact. The mean is double. Metric windows or systems that require exact 128-bit summed nanoseconds should specify a separate interoperable wide-integer contract before exporting. Do not treat this type as a wire-schema or binary ABI. For huge counters, rate is converted to double only after an exact uint64 delta.

### Merge policy

`merge_sufficient` merges only count, sum, min and max of **two contiguous, disjoint, complete, exactly identical metric identities** (including unit, population, owner, clock, session/reset and kind). Reject gaps, overlaps, different reset identities and capacity-evicted windows. The function deliberately returns **no P95/P99**. Combining quantiles across owners/windows requires a separate compatible histogram or raw sample merge; **never average P95s**. A histogram's bucket boundaries, censorship policy and error bound would need coordinator approval before becoming a shared v1 schema.

## Example synthetic offline snapshots

The following are illustrative typed-field excerpts, **not real telemetry**, and not the future HTTP schema:

```text
healthy 60 FPS, 2 s:
  counter delta=120, elapsed_ns=2000000000, rate=60.0/s, status=Valid
  completed intervals=120, min=16666666 ns, max=16666667 ns
same mean 60 FPS, four 80 ms bursts:
  counter rate=60.0/s, inter-frame P95=80000000 ns, max_gap=80000000 ns
no samples yet:
  availability=NeverObserved, success_count=0, mean=null, p99=null, coverage=null
counter reset inside window:
  counter values 100 -> 120 -> 4, status=ResetBoundary, rate=null
```

## Safe integration and boundaries

Proposed future TelemetryModel adapter: snapshot/copy **scalar** owner counters at a throttled observation cadence (~250 ms or 1 Hz), push into a dedicated `CounterSeries` for each declared reset domain, and query snapshots only on a diagnostics worker. For CV/overlay events, record a scalar duration **only on successful matched-key path** while separately feeding timeout/missing/skipped events from the authoritative Exact Sync lifecycle producer. Never rederive lifecycle outcomes from UI FPS. Don't duplicate an authoritative counter or add per-frame heap/JSON/sort/disk/network work to decode, render or CV callbacks. TelemetryModel and ExactSyncDiagnostics have intentionally not been modified in this PR.

The future fixture adapter for Agent B can map `data_origin=synthetic`, `host_role`, `session_id`, `clock_domain`, decimal-string `t_monotonic_ns`, and a metric-specific `type`/`attributes` into these typed observation methods **after validating units/reset scope and filtering frame keys**. This core does not define or change the shared `diag-fixture/v1` JSON envelope, frame identity protocol, HTTP DTOs or OpenAPI contract; correlation belongs to the coordinator/lab layer. Agent C owns Exact Sync lifecycle classification.

**Hardware acceptance not attempted:** real RTP packet gaps, decoding/Vulkan presentation, Jetson inference cadence, cross-host clock calibration and hot-path profiling on the target platforms remain integration gates.
