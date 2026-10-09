# Telemetry Analytics Responsibility Model

**Status:** design decision / proposed implementation, 2026-10-10.
**Scope:** Ubuntu UNIGINE and MediaMTX; Jetson Orin Nano CV pipeline; Windows Reg; Ubuntu lab diagnostic collector and optional LLM agent.
**Companion specifications:** DIAGNOSTICS_API_SPEC.md; DIAGNOSTICS_AGENT_README.md; TESTBED_INTELLIGENCE_VISION.md.
**No executable code is changed by this document.**

## 1. The boundary

**Measure at the owner. Calculate primary statistics at the owner. Correlate and score centrally. Interpret evidence through an LLM.**

The agent MUST NOT consume continuous unfiltered logs or repeatedly generate temporary scripts merely to calculate FPS, min/mean/max, P95, P99 or drop rates. Those are deterministic, unit-tested services using typed telemetry. Scripts may be used for novel offline investigations against immutable exported bundles; successful routines become reusable named queries.

A separate fault runner alone can restart an allowlisted Ubuntu-local process with approval. Diagnostic API and MCP are read-only.

## 2. Layered responsibilities

| Layer | Where | Responsibilities | Explicit exclusion |
|---|---|---|---|
| Measurement | UNIGINE, MediaMTX, Jetson, Reg | Instrument exact local event boundaries, FrameKey, session and reset identity, clocks, success/failure counts, bounded event IDs | No LLM, no network or disk I/O in critical frame callbacks |
| Local statistics | Per host off critical threads | Named and versioned metric definitions, rolling counts/rates, count/sum/min/max/histograms, 1 s/10 s/60 s views, freshness and population labels, anomaly threshold transitions | No cross-host causal claims |
| Lab deterministic analyzer | Ubuntu | Join events by experiment ID, session identity and source FrameKey; use UTC uncertainty for cross-host time alignment; compute baseline delta, exact coverage, rule/contract violations and reproducible PASS/FAIL/INCONCLUSIVE | No LLM-dependent acceptance |
| Data store and query | Lab / bounded host rings | Retain detailed raw samples/events separately from compact summaries; event-indexed evidence, pre/post-fault capture, historical experiment manifests, indexed log excerpts | Never dump full history as a tool response |
| HTTP / MCP tools | Reg and Jetson read-only HTTP; adapter near Codex | Typed fixed tools, response budgets, capability discovery, bounded filters/cursors and evidence references | No primary statistics re-computation, no arbitrary code execution |
| LLM | Operator host | Hypotheses, selecting next targeted query, discussing alternative explanations, preparing reports with evidence_refs, requesting approved tests | No invented values, no core statistical authority, no continuous log feed |
| Test runner | Separate approved local test runner | Fixed allowlisted actions, timeouts, audit, restoration and emergency stop | Not mounted as general Reg Diagnostic API control |

## 3. Statistics calculated by local owners

**Ubuntu UNIGINE source:** actual encoded and published frame rates, FrameKey SEI completeness, frame pacing/gaps, encoder stage timing, source restarts/epoch changes, publishing errors. **MediaMTX:** use existing control GET API and Prometheus metrics for publisher/reader health and RTP packet loss/error counters. Packet loss is not interchangeable with lost decoded video frames.

**Jetson CV:** video receive/decode FPS; queue wait/depth; stage-specific preprocessing, inference, postprocessing and tracking durations; CV output FPS; valid-zero-target cases; CUDA/GPU utilization, RAM, temperature and throttling from a separate bounded sampler; CVM1 prepared/sent/socket-failed/dropped counts and last source FrameKey; workload/model/batch/revision. Socket send success does not prove Reg receipt. Different CV models, resolution, batch size and source periods have distinct metric populations. If a sensor or instrumentation hook is not present, mark unsupported rather than reporting zero.

**Windows Reg:** demux/decode/Raw present counts and rates, last-frame age, frame interval/jitter/stalls, Vulkan transitions, receiver CVM1 accepted/invalid/out-of-order/duplicate/gap counters, strict FrameKey match and eligible-frame denominator, missing/late/evicted/never-observed CV results, Overlay configured/effective delay and buffer pressure, recorder queues/drop/write and NetImgui sessions. Current successful-match-only ExactSyncDiagnostics wait percentiles are NOT representative of all metadata latency and must say matched_only.

The same measurement producer supplies the GUI, HTTP, metrics exporter and evidence store. Avoid conflicting calculations of "FPS" across components with different counting points.

## 4. Primary statistics and mathematical safeguards

Every local duration population should expose:
- count, sum, min, max and avg (only count>0);
- mergeable latency histogram plus display P50/P90/P95/P99 with documented resolution;
- success/timeout/cancelled/dropped/never-observed sample counts as separate classes;
- window_start/window_end, local clock domain, sample_count, sample population, model/session/epoch/reset scope, freshness and quality status;
- rates derived from counter delta / elapsed local time within the SAME reset scope.

Recommended window views: 1 second for reactive health (not stable tail quantiles), 10 seconds for short disturbances, 60 seconds for typical percentiles and configurable experiment windows. Even at 60 FPS, a one-second P99 is based on only ~60 observations and is not a robust tail estimate.

**Do not average per-host or per-window P95**. Merge compatible histograms and compute a new percentile, or retain event samples and compute exact quantiles offline. Do not average averages or rates across different counts/windows: combine sum/count and numerator/denominator correctly. Different model, pipeline or epoch populations are grouped or excluded explicitly. Min/max identify extremes but are outlier-sensitive; P95 hides the top 5%; always report timeouts/drops/coverage.

Prometheus native histograms or matched classic histograms can support server-side aggregation. A lightweight in-process bounded ring and exact background percentile calculation is acceptable for an individual metric if there is no need to merge it, but publish its sample population and period clearly. Use deterministic algorithms (bounded histograms, count/sum, Welford online variance) in a low-priority thread; never do expensive sorting, I/O or JSON serialization in a decoder/inference callback.

## 5. Agent context and cost budget

**The LLM sees diagnoses, not time series or logs by default.** Proposed tiered interface:

| Agent operation | Typical bounded output | Reason |
|---|---|---|
| lab.overview(5m) | target <=2 KiB JSON, <=5 most important episodes | One compact health snapshot for three machines |
| lab.list_episodes | <=5 event windows, counts, severity, evidence IDs | Which disturbances require investigation |
| lab.metric_stats(metric,window) | one already calculated distribution, completeness and scope | Min/avg/max/percentiles without scripts |
| lab.compare_windows(baseline,test) | paired change, reset boundary and measurement conditions | Objective regression comparison |
| lab.frame_lifecycle(epoch,frame_id) | one retained source-frame chain and missing boundaries | Exact sync debugging |
| lab.log_excerpt(event/window,limit=20) | <=20 redacted and selected lines, paged | Hypothesis-specific context |
| lab.evidence_bundle(run_id) | only artifact reference and digest | Complete offline analysis without polluting chat |

Additional proposed bounds: <=8 KiB per typical drill-down, <=60 plot points per response, cursor pagination with explicit truncation, rate limits and per-investigation request quota. Budgets are **defaults for profiling**, not performance guarantees. Output size in bytes is not exactly token count. If higher detail is required, export locally and process offline with deterministic SQL/Python rather than pasting into chat.

The local collector watches metrics at ~1 Hz and emits a deduplicated **episode** on anomaly/transition/recovery. The LLM runs on operator request or on meaningful episode changes, not for every metric tick. A healthy five-minute interval should produce zero unsolicited LLM calls.

## 6. Log reduction without losing evidence

Use structured event types first. For legacy MediaMTX/UNIGINE text, template mining (e.g. Drain3) or strict parser rules group repeated warnings, tracking first/last seen, count, numeric parameters, maxima and sample lines. **Never use LLM summarization as the sole means of retaining facts.**

Example: 500 RTP loss warnings in a five-minute trace become an episode with 500 occurrences, summed RTP packet-loss delta (only if values are proven non-overlapping), peak burst, time range, affected stream/session and 2-3 exemplar lines plus evidence_refs. Their original log excerpts stay in the bounded archive and can be retrieved specifically. Missing correlation with Windows Raw or Jetson is explicitly noted rather than inferred.

Store compact counters and histograms continuously; keep a bounded high-resolution ring; when an incident occurs, preserve a proposed 30-second pre-trigger and 60-second post-trigger evidence slice. Include manifest, config/build fingerprints, FrameKey, sample gaps and source checksums. No video or full logs in MCP responses by default. Sampling policies must avoid deleting rare critical events.

## 7. Deterministic anomaly and conclusion boundaries

The central analyzer provides known rules, baseline detection, stream/source health graph and reset-aware aggregation. Example: MediaMTX reader connected, Reg decode counter frozen, Raw no-signal watchdog fired -> likely failure between RTSP ingestion and decode; inspect packet age and decoder errors. Do not call this proof of a network cause.

LLM generates alternative hypotheses and asks the next discriminating question, but a scriptable evaluator determines if predeclared thresholds were satisfied. A final report distinguishes measured observations, deterministic deductions, hypotheses and verified test results. Every claim points to evidence refs and exposes important absent/stale data.

## 8. Tests and ownership acceptance

- Run deterministic reference vectors for count/mean/min/max/quantiles, hist precision error, resets, zero samples and mixed-epoch periods.
- Validate a long unstructured-log flood cannot exceed MCP output limits, while evidence remains available for bounded historical queries.
- Test fake valid CVM1 with zero targets, missing frame, dropped/late metadata and sender accepted without Reg receipt.
- Benchmark Jetson inference and Windows Raw P95/P99 with diagnostics enabled/disabled; no meaningful critical-path regression allowed.
- Disconnect the LLM and confirm collection, summary, anomaly detection, tests and PASS/FAIL/INCONCLUSIVE continue unchanged.
- Track tokens per incident, API calls, incident localization accuracy, unsupported claims, operator time and missing-data tolerance.

## 9. Related work and technical sources

- RCACopilot (Microsoft, EuroSys 2024): curated diagnostics collection and explanatory RCA. https://www.microsoft.com/en-us/research/publication/automatic-root-cause-analysis-via-large-language-models-for-cloud-incidents/
- OpenRCA (Microsoft, ICLR 2025/2.0): hundreds of incidents, tens of GB of heterogeneous telemetry; retrieval is necessary. https://microsoft.github.io/OpenRCA/
- AIOpsLab (Microsoft, MLSys 2025): instrumentation, fault injection, typed agent tools and deterministic evaluation. https://microsoft.github.io/AIOpsLab/
- Grafana Assistant Investigations: query-based hypotheses and evidence-linked data source views. https://grafana.com/docs/grafana-cloud/platform/grafana-assistant/platform/investigation/
- Prometheus histogram statistics and recording rules: https://prometheus.io/docs/practices/histograms/ ; https://prometheus.io/docs/practices/rules/
- OpenTelemetry Collector filtering and transformation: https://opentelemetry.io/docs/collector/transforming-telemetry/
- Drain3 legacy log template mining: https://github.com/logpai/Drain3
- NVIDIA DeepStream performance and stage latency measurement as illustrative methodology, NOT dependency: https://docs.nvidia.com/metropolis/deepstream/9.1/sdk-api/structNvDsAppPerfStruct.html ; https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/group__ee__nvlatency__group.html
