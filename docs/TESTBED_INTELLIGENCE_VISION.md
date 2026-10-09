# Reg Testbed Intelligence — Foundational Vision and Research Roadmap

**Status:** FOUNDATIONAL PROJECT DIRECTION / DESIGN, **not an implemented capability**.
**Date:** 2026-10-09
**Scope:** Reg's three-node UAV simulation / semi-physical test stand: Ubuntu (UNIGINE 2.22 SIM + MediaMTX), NVIDIA Jetson Orin Nano (computer vision + CVM1), and Windows (Reg video/Exact Sync/telemetry/recorder).
**Companions:** [MVP architecture](MVP_ARCHITECTURE.md), [Diagnostic API contract](DIAGNOSTICS_API_SPEC.md), [agent operations guide](DIAGNOSTICS_AGENT_README.md), [restart acceptance](RESTART_RECOVERY_VALIDATION.md), [FrameIdentity](FRAME_IDENTITY_SEI.md).

## 1. Project commitment

**Intelligent diagnostic observability is a first-class pillar of the Reg testbed, not a GUI add-on or ad hoc tool for debugging.**

Reg's four long-term engineering pillars:

1. **Real-time visual observation:** NVIDIA hardware H.264 decode, Vulkan presentation, independent newest-frame Raw, and delayed CV Overlay.
2. **Frame-accurate synchronization:** explicit H.264 source FrameKey (stream_epoch, frame_id), strict video/Jetson CVM1 matching, replayable evidence.
3. **Cross-host observability and reproducibility:** explicit diagnostic data contracts, state/failure/recovery events, bounded traces, configuration/build provenance, shared experiment IDs, and replayable test evidence.
4. **AI-assisted diagnosis and experimental verification:** agents inspect reliable cross-host evidence, formulate competing failure hypotheses, propose targeted tests, evaluate outcomes and produce audit-ready conclusions.

Pillars (3) and (4) influence the design of all future Reg subsystems. New features are expected to be diagnosable remotely without screenshots or human log forwarding.

### This is NOT

- A requirement to deploy a language model on the Jetson or in Reg's Vulkan/RTSP data plane.
- An AI-based flight-control, vehicle actuation, weapons/control-loop, or autonomous physical test system.
- A promise that a model knows the cause of every fault, or that text generation constitutes proof.
- A claim that an HTTP API, MCP adapter, centralized collector, autonomous experiment runner, or AI analyzer is already implemented.
- A substitute for deterministic validation, hardware observation or human authority over disruptive actions.

The current rig is a **specific UAV simulation and compute-in-the-loop testbed**. Do not claim full aircraft flight-controller HIL, full aerodynamic fidelity, or flight safety validation until that hardware/software loop and corresponding tests are added.

## 2. Core scientific/engineering question

Can a *hybrid evidence-grounded system* combining deterministic instrumentation, exact video frame identity, cross-host telemetry and LLM hypothesis reasoning locate faults and verify recovery in a heterogeneous UAV simulation rig **more accurately, faster, more reproducibly and with lower operator workload** than manual inspection, raw-log-only LLM analysis, and deterministic monitoring alone?

The investigation is centered on:
- **Cross-host event correlation with heterogeneous clocks and partial observability**, not naïve timestamp subtraction;
- **FrameKey-aware causal constraints** for video, CV inference and UDP metadata;
- **Structured evidence provenance and abstention**, not persuasive but unsupported natural-language narratives;
- **Active diagnosis via safe, bounded fault injection** and controlled restarts, not uncontrolled "self-healing";
- **Hardware-tested performance** and explicit limits to generalization across models, network conditions, builds and Jetson load.

This is a proposed research direction. Novelty must be established with a proper literature review and comparative experiments; LLM log analysis, agent diagnosis and UAV HIL are existing research fields.

## 3. Three planes, separate authorities

~~~text
        Evidence / interpretation plane (not time-critical)
       +----------------------------------------------+
       | Agent (e.g. Codex) + local MCP adapter       |
       | hypotheses | evidence graph | test planning |
       | evidence citations | PASS/FAIL/INCONCLUSIVE |
       +------------------+---------------------------+
                          | read-only MCP tools / HTTP
                          v
       +----------------------------------------------+
       | Observation plane                            |
       | per-host collectors | Diagnostic API /v1    |
       | typed snapshots | structured events         |
       | ring traces | optional time-series store     |
       | immutable, redacted experiment evidence      |
       +---------+-----------------+------------------+
                 |                 |
        Ubuntu / MediaMTX      Jetson CV             Windows / Reg
            (read-only)        (read-only)            (read-only)
                 \                 |                  /
                  +---- measured component states ----+
                                ||
                      no backpressure or commands
                                ||
       +----------------------------------------------+
       | Real-time video/CV plane                      |
       | UNIGINE -> H.264 SEI -> MediaMTX -> Reg Raw  |
       |                 \-> Jetson -> CVM1 -> Overlay |
       | Vulkan decode/present; FrameKey Exact Sync   |
       +----------------------------------------------+

       Separate OPTIONAL control/test plane:
       human-approved Ubuntu-local allowlisted test runner;
       audit trail, timeouts, rollback, emergency stop
~~~

**Authority rules:**
- **Measured facts** come from component instrumentation/collectors; collection must carry source, time, schema/build and freshness.
- **Deterministic analysis** computes deltas, checks FrameKey compatibility, detects timeout transitions, evaluates acceptance assertions and localizes bounds of uncertainty.
- **LLM** generates plausible explanations and follow-up questions, not unverified metric values, deterministic verdicts or runtime control output.
- **Test runner** applies only explicitly approved, bounded and allowlisted local test actions; authorization is separate from observation.

## 4. Evidence lifecycle and auditability

### 4.1 Canonical experiment record

One experiment consists of a manifest, observations, actions and a verdict:

- experiment_id / test_run_id, scenario_id and scenario version, hypothesis and acceptance criteria declared **before** action;
- hardware/software inventory and versions/build SHA on every host (plus CV model/revision where available);
- effective experiment configuration, source frame rate/resolution/transport, manual Overlay delay, random seed when meaningful;
- process_session_id, decoder generation and FrameKey (both stream_epoch and frame_id);
- UTC event timestamps with sync quality/uncertainty and each host's locally measured monotonic durations;
- raw structured data or digests/references, sampling gaps, freshness and unavailable-data reasons;
- approved action log: actor, target service, method, request/return time, precondition, result and safety stop;
- deterministic assertions, competing hypotheses, the specific observations supporting/refuting them, and calibrated uncertainty;
- PASS/FAIL/INCONCLUSIVE and a machine-verifiable reason; independent review annotation / later ground truth.

**Do not claim causal proof from mere temporal coincidence.** Sequence is evidence, not sufficient causality. A stronger inference requires component contract, causal graph constraints and/or intervention with relevant controls.

### 4.2 Frame-level correlation

Use canonical H.264 SEI FrameKey=(stream_epoch, frame_id). Reg and Jetson copy the key from the source video; never synthesize an independent decoder counter. Use experiment/test ID for run-level grouping, independent of FrameKey. Video decoding, Raw presentation, Jetson inference, CVM1 send/receive and Overlay decisions should each expose their own independently recorded boundary measurements. A frame with zero detections is not a missing metadata packet.

Use paired records to distinguish:
- source frame never emitted vs MediaMTX/transport loss;
- demuxed packet received but hardware decode stalled;
- source decoded and Raw shown but CV inference unfinished;
- Jetson sent a CVM1 packet yet Reg did not accept it;
- Reg accepted metadata but missed its exact-frame overlay deadline;
- frame dropped because of metadata absence vs buffer eviction vs missing SEI.

Cross-host monotonic clock timestamps **must never be directly subtracted**. Exact identity and local durations remain valid with unsynchronized host clocks. UTC event alignment is approximate unless timestamp uncertainty has been measured.

### 4.3 Evidence-backed conclusions

Every finding should have:
- claim, severity, and classification (**observation**, **deterministic inference**, **hypothesis**, or **verified intervention outcome**);
- evidence_refs linking to bounded API snapshots/events/frame records, not a synthetic claim;
- alternative explanations and missing observations;
- confidence/uncertainty that reflects evidence limitations, not an arbitrary "LLM confidence" percentage;
- next best discriminator: cheapest safe measurement or experiment that would separate competing hypotheses.

LLM must be allowed to say "**insufficient evidence**" and request additional measurements. Unsupported assertions are a first-order quality failure.

## 5. Intended operator workflows

**A. Explain live degradation:** Operator asks "Raw has video, why does CV Overlay skip frames?" Agent retrieves matched vs eligible FrameKeys, last metadata age, jetson inference queue/time, CVM1 gap and buffer capacity. It reports bounded hypotheses and concrete next checks, or abstains if Jetson exporter is absent.

**B. Validate restart recovery:** Operator approves a MediaMTX restart on Ubuntu. Runner records action; API records MediaMTX readers/publisher, FFmpeg decoder generation, reconnect events, Raw/NetImgui and CV changes. Deterministic evaluator checks predeclared recovery/stability bounds, agent summarizes evidence and unresolved faults.

**C. Compare parameter sweeps:** At controlled source rate and Jetson conditions, vary *manual* Overlay delay across predeclared settings. Compare deadline misses, buffer evictions, match coverage, frame age and inference workload. Agent explains trade-offs. This supports a later Adaptive Delay **Observe** implementation; Auto is not implied.

**D. Regression/incident package:** After a change in UNIGINE, Reg or Jetson, export a sanitized manifest and timeline. Another agent or engineer can reanalyze the same immutable dataset without re-running the whole rig.

**E. Propose a discriminating test:** Given competing hypotheses "Jetson inference is late" vs "UDP CVM1 delivery is failing", collect Jetson decode/inference/send durations and Reg reception for matching FrameKeys before suggesting a controlled network perturbation. Never diagnose from screenshots alone.

## 6. Minimum valuable product (MVP) and milestones

| Gate | Deliverable | Exit evidence |
| --- | --- | --- |
| T0 | Cross-host evidence bundle using existing Reg CSV/log, MediaMTX GET APIs and optional Jetson sampler | One experiment yields manifest, timestamps, bounded observations and explicit unknowns |
| T1 | Read-only Reg Diagnostic API (/v1/capabilities, /v1/status, /v1/snapshot) | Ubuntu agent retrieves fresh, correctly scoped Windows Reg RTSP, Raw, CV and Exact Sync evidence without screenshots |
| T2 | Typed events, selected frame traces and Jetson CV exporter | Controlled raw/video/CV failure can be localized at its measured stage using identical FrameKeys |
| T3 | Lab MCP adapter + deterministic assertions + evidence report | Agent can query all three hosts, while verdict is machine-checkable and no model response is trusted as a fact |
| T4 | Approved local fault-injection/test runner | Repeated restart experiments produce stable, auditable PASS/FAIL/INCONCLUSIVE; unexpected changes are blocked |
| T5 | Research dataset and baseline comparison | Reproducible labeled failure corpus and blind evaluation against manual/rules/log-only/hybrid baselines |
| T6 | Adaptive CV Overlay Delay Observe | Censored+late frame analysis validated on real Jetson; suggested delay is measurable but does not change playout |
| T7 | Optional adaptive Auto / broader test automation | Only after separate stability, resource, safety and human approval gates |

A milestone is not finished because an LLM generated a plausible narrative. Implementation, provenance checks, repeatability and end-to-end hardware results must be demonstrated.

## 7. Benchmark and scientific validation plan

### 7.1 Hypotheses (to test, not already established)

- **H1:** Structured frame-correlated multi-host evidence improves top-1 root-cause localization and reduces ungrounded causal assertions compared with raw-log-only LLM analysis.
- **H2:** Hybrid deterministic assertions + an LLM improves investigation time/operator effort versus manual triage and rules-only monitoring without sacrificing verdict reliability.
- **H3:** An agent instructed to abstain on stale, missing or causally ambiguous observations yields fewer false confident diagnoses than an agent given raw summaries without provenance.
- **H4:** Controlled experiments with source/Reg/Jetson session and epoch tracking improve repeatability and reliability of RTSP restart recovery assessment.
- **H5 (later):** Capturing late/missing/censored CV frame observations produces more useful adaptive-delay recommendations than matched-only P95 of completed pairs.

### 7.2 Comparators / ablations

Measure on **exactly the same held-out trials**, under the same hardware and acquisition conditions:
1. Human operator with conventional GUI, raw logs and screenshots;
2. Deterministic rule/state-machine checks on the structured data, **without LLM**;
3. LLM with raw text log excerpts only;
4. LLM with structured per-host metrics/events but **without** FrameKey lifecycle;
5. Full hybrid deterministic + FrameKey-aware + evidence-grounded LLM;
6. If feasible, full method without active intervention or with missing one host, to measure sensitivity.

Declare allowed model versions, prompts, sampling parameters, token/cost budgets, time budgets, retry counts and permitted observations before evaluation. Prevent contamination: split incident families/versions or test scenarios between examples and blind trials; archive configurations and datasets.

### 7.3 Controlled fault catalog (initial proposal)

**Source/transport:** UNIGINE publisher not started; UNIGINE restart/epoch reset; MediaMTX stopped/restarted; source before/after server startup; temporary packet loss/reorder; RTP fragmentation/corrupt access units; wrong RTSP endpoint.

**Reg:** FFmpeg demux stalled; codec/hwaccel initialization failure; Raw present/device/surface failure; missing or nonmonotonic FrameIdentity; overlay buffer eviction; recorder backpressure/disk exhaustion; NetImgui disconnected independently of Raw.

**Jetson/CVM1:** Jetson inference slowdown/thermal throttling; CV queue backlog; sender would-block; packet loss/reorder/duplicates; stale epoch; zero valid targets; metadata arrives after playout deadline; temporary receiver shutdown.

**Ambiguity/control:** healthy no-fault trial; coincidental but non-causal warnings; clock drift; stale collector while video is healthy; multiple interacting faults; intentionally unobservable cause; counter reset midway.

Ground truth is the *injected fault* plus manually checked observable effect, NOT the LLM's conclusion. For each case record fault start/end, intervention, permitted measurements, exposure duration and physical effects if any. Begin with software/service/network perturbations in a controlled isolated rig; no uncontrolled hardware fault injection or live-flight trials.

### 7.4 Metrics and statistical protocol

Report per fault category and overall:
- Detection precision/recall and false alarms/hour under healthy operation;
- Root-cause top-1/top-3 accuracy, hierarchical localization (host -> subsystem -> stage) and confusion matrix;
- Unsupported claim rate, evidence-reference validity, cross-clock/FrameKey violations, and correctness of **INCONCLUSIVE**;
- Time-to-detect, time-to-localize, time-to-verified-recovery, operator interventions, and proportion of tests reproduced;
- Quality of proposed discriminating experiment, authorization violations (target 0), safe stop success;
- Impact on Raw P50/P95/P99 frame/render timing, decoder drop/reconnect and recorder loss with diagnostics on/off;
- Network/API overhead, Jetson resource consumption, LLM token/cost/latency and missing-data robustness.

Pre-register primary metrics, evaluation thresholds, sample sizes and stopping rules in a later benchmark protocol before seeing results. Use repeated runs with representative random seeds and cross-condition holdouts; confidence intervals / paired comparisons where valid. The eventual publication should expose negative results, test artifacts, model versions and hardware constraints.

## 8. Operational safety, limits and data governance

- The LLM is off the critical flight/video path; a model/API outage must not stop decode, display, recording, or CV source.
- Read-only query authority is the default. Separate guarded test-runner actions require explicit authorization, fixed allowlist, timeout, automatic cleanup/rollback where possible, and complete audit trail.
- No arbitrary command execution, destructive filesystem access, sensitive RTSP URL disclosure, privileged remote process control or unreviewed code deployment through the diagnostic interface.
- Test and diagnostic payloads are untrusted. Logs and CV labels can contain prompt-injection text; treat these as evidence, not agent instructions.
- Evidence should be sanitized before any external LLM call; default bundle contains structured metadata/events, not raw video. Sensitive content should remain local unless explicitly authorized.
- A trustworthy diagnostic response must declare measurement scope, availability, freshness, clock domain, reset generation, producer and relevant uncertainty.
- Prevent instrumentation perturbation: bounded buffers, independent workers, measured CPU/GPU/network budgets, no per-frame synchronous HTTP or disk operations.

## 9. Architecture integration and maintenance obligations

All Reg contributors/agents must:
- Treat diagnosability and evidence coverage as part of feature acceptance, together with performance and functional correctness.
- Add/adjust metrics, structured events, schemas, event definitions and tests **in the same PR** as new significant runtime behaviour.
- Update [Diagnostic API contract](DIAGNOSTICS_API_SPEC.md), [agent README](DIAGNOSTICS_AGENT_README.md), and the machine-readable schema (once implemented) whenever observable semantics change.
- Preserve the single-source metric principle: UI, HTTP API, MCP adapter and evidence exports share authoritative instrumentation.
- Never make LLM-based reasoning the only means to determine correctness in CI or hardware acceptance; machine-checkable evidence and thresholds take precedence.
- Distinguish **currently implemented**, **planned**, and **experimentally validated** functionality in all documentation and release notes.

## 10. Research positioning and related work (orientation, not novelty proof)

Existing research includes LLM-assisted log anomaly analysis and root-cause analysis in distributed systems, as well as UAV simulation/HIL and AI-supported diagnostics. Reg's *candidate* differentiator is evidence-grounded, frame-accurate, cross-host diagnosis + active verification in one heterogeneous simulated-UAV video/CV rig. This must be demonstrated against relevant baselines, not asserted as universally unique.

Selected public references as of 2026-10-09:
- Barata et al. (2026), "Anomaly detection and root-cause identification in microservices: a survey," Cluster Computing, DOI 10.1007/s10586-026-06095-9. https://link.springer.com/article/10.1007/s10586-026-06095-9
- "Microservice logs analysis employing AI: A systematic literature review" (2026), Journal of Systems and Software, DOI 10.1016/j.jss.2026.112786. https://www.sciencedirect.com/science/article/pii/S0164121226000208
- "SPADE: Simulator-assisted Performability Design for UAV-based monitoring systems" (2025), Future Generation Computer Systems. https://www.sciencedirect.com/science/article/pii/S0167739X25002626
- Halba, Cooper and Bellingham (2026), "A Simulation Platform for AUV Fault Recovery: Exploring LLM-Based Diagnostic Strategies," arXiv:2609.20620. Note: AUV is underwater, not a UAV; cited for rigorous repeated evaluations of LLM fault diagnosis, not equivalent test hardware. https://arxiv.org/abs/2609.20620
- "A UAV Testbed for Diagnosing Hardware Vulnerabilities: Quantifying Sim-to-Real Discrepancies in PX4 Flight Logs" (2026), Sensors 26(10), 3188, DOI 10.3390/s26103188. https://www.mdpi.com/1424-8220/26/10/3188

## 11. Decision record

**Accepted as a long-term design goal:** cross-host evidence, agent-readable diagnostics and research-grade reproducibility are foundational Reg capabilities.

**Not authorized by this document:** exposing diagnostic endpoints on untrusted networks, installing cloud AI agents, granting process-restart privileges, modifying existing RTSP/CV processing logic, automatic adaptive delay, or claiming scientific novelty/experimental results without evidence.
