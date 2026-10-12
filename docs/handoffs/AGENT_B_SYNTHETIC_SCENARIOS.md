# Handoff B — Deterministic Synthetic Diagnostics Lab

**Assignment:** independent scenario generator and oracle corpus for offline Reg diagnosis.
**Suggested branch:** test/diag-scenario-lab.
**Priority:** P0; start immediately without Jetson, UNIGINE or RTSP.
**Scope:** executable generator/test scripts plus fixtures, NOT changes to Reg decode/render.

## Goal

Create a tiny, repeatable offline virtual diagnostic stand producing realistic typed observations from synthetic Ubuntu UNIGINE/MediaMTX, Jetson CV/CVM1 and Windows Reg components. Every scenario must include **injected ground truth** for deterministic assertion checks. Fixtures must never masquerade as real hardware data.

## Prior context

Reg uses exact FrameKey=(stream_epoch, frame_id) from H.264 SEI, UDP CVM1 metadata, Raw newest-frame semantics, bounded delayed Overlay with precise matching and 150 ms manual playout delay. Existing tests cover frame sync, CVM1 bridge/UDP and reconnect, but there is no test corpus that generates multi-stage diagnostic events with adjustable faults and virtual time. Read docs/handoffs/README.md shared interchange contract before creating JSON.

## Ownership and constraints

OWN:
- tools/diagnostic_scenarios/** (Python 3 standard library is a reasonable zero-dependency solution; C++ optional only if beneficial).
- tests/fixtures/diagnostics/** (JSON manifests, JSONL event observations, deterministic expected facts).
- Own script/test README under tools/diagnostic_scenarios/.

DO NOT EDIT:
- CMakeLists.txt and src/diagnostics/stats/** (Agent A);
- src/diagnostics/ExactSyncDiagnostics.hpp or existing Exact Sync tests (Agent C);
- src/main.cpp, OverlayFrameBuffer, runtime UI, HTTP API, Launcher.

Avoid external packages and GPU/network requirements. Use argparse/unittest if Python. Do not invent a live API or request actual Jetson data.

## Offline schema contract

Read docs/handoffs/README.md for frozen minimal diag-fixture/v1 observation envelope. Use local monotonic nanosecond strings with an explicit clock_domain; each host has its own clock and process/session. JSON uint64 FrameKey members are decimal strings. Every event is data_origin=synthetic. Provide manifest with scenario_id, version, seed, base simulated FPS/duration, configured CV delay, injected fault code/start/end, expected deterministic statistics and relevant reset/session transitions.

Prefer one JSONL event per stage milestone with typed event name and optional attributes; maintain exact FrameKey cross-host even if hosts have different local clocks. Ground-truth *cause* is explicitly test-only metadata, not a claim a production analysis engine should be given at diagnosis time. Keep a redacted/public observation projection with ground truth hidden for future LLM blind evaluation.

## Minimum scenarios

- healthy 60 FPS steady input and 120 FPS variant with jitter-free FrameKeys;
- irregular 60 FPS average with a long 180 ms gap/bursts, contrasted to steady;
- source/UNIGINE restart that renews stream epoch and resets frame IDs; MediaMTX restart where source epoch may remain stable;
- Reg demux/decode stall then reconnect; decoded frames stop while media transport may continue;
- CV metadata arriving before Reg decode, after playout deadline, after drop, never arriving, duplicated and reordered;
- CVM1 valid zero targets vs missing packet; sender success with simulated receiver loss;
- Jetson inference slowdowns and growing queue wait, while Raw remains healthy;
- Overlay delay sweeps 50/100/150/200/300 ms with drop/eviction outcomes;
- independent NetImgui disconnect or recorder saturation; stale diagnostic service with otherwise healthy video;
- synthetic multi-host clock offset/drift and counter/session reset, so naive cross-host timestamp subtraction fails;
- ambiguous/no-fault episode plus repeated-log flood for future context budget checks.

Keep each case small (e.g. seconds) and generate long stress traces on demand rather than storing huge fixtures in Git.

## Determinism and validation

- Same seed + scenario version -> byte-for-byte identical fixture content, including event ordering, frame identity and expected assertions.
- Virtual-clock increments are deterministic; no dependence on system time, sleeps, random default seed or nondeterministic dictionary order.
- Validate schema, fixed ordered events, monotonically advancing FrameKey per source epoch where expected, intentional reset cases, matching CV keys, no accidental inter-host clock equality assumption.
- Check known expected rates/window aggregates and scenario cause fields separately.
- Include tests for malicious/invalid event values, empty windows, 64-bit ID beyond JavaScript safe integer range, random jitter at configured seed and bounded output.
- Support command line list/generate/verify and optional exported short human-readable scenario summary. No unbounded stdout.

## Integration contract

Agent A will produce a C++ core stats interface but is not required as a build dependency for this handoff. Agent C will instrument Exact Sync; your fixture stream must include all inputs required to exercise the resulting lifecycle. Coordinator will later write the adapter tying fixtures to production logic. Communicate any proposed shared schema change **before** landing it.

## Required result

Separate small PR with generator, manifest/event schema README, generated golden fixtures, deterministic verifier and unit test outcomes. Provide at least one clean and one corrupted/late-CV example with exact expected conclusions. Declare all values synthetic, no hardware claims.

## Completion message to coordinator

Give seed/version, commands to regenerate and validate fixtures, scenario list, test output and expected integration adapter shape; provide branch, commit and PR URL.
