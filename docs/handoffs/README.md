# Reg Diagnostics — Parallel Agent Coordination

**Status:** WORK ALLOCATION / HANDOFF; no implementation is implied.
**Last checked:** 2026-10-12.
**Repository:** kxsxlxv/Reg
**Active runtime base:** feat/netimgui-russian-detr, reviewed commit 82641a1760361dbc5d46f441eb03ee1ddd34452a
**Architecture/docs PR:** #33, docs/diagnostics-contract-and-agent-guide, currently a DRAFT, not yet merged.
**Project coordinator:** the original Reg ChatGPT conversation. Separate implementation chats have no implicit shared context; use the linked handoffs verbatim.

## Mission

Build offline-testable, low-overhead observability for Reg: local measurement/statistics, synthetic reproducible fault fixtures, precise Exact Sync event accounting. Next wave: headless read-only HTTP, incident episodes, evidence archives, thin MCP. Do not attempt hardware-validity claims without actual UNIGINE/RTSP and Jetson.

## Execution pattern

**Preferred:** Review/merge architecture-only PR #33 into feat/netimgui-russian-detr first. Then all independent agents branch from the same updated feat/netimgui-russian-detr tip. Each must create an isolated branch and ideally an isolated worktree / Codex workspace. Each agent owns only its documented paths; all work is submitted as a separate PR, not merged by the agent. No two agents edit the same working tree.

**If work must start while #33 remains draft:** agents may branch from docs/diagnostics-contract-and-agent-guide and target their PR at that documentation branch as a temporary stacked base. DO NOT merge those implementation PRs into the documentation branch until PR #33 is integrated. The coordinator later rebases/retargets those branches onto the actual runtime integration branch. Avoid accidentally inflating #33 with executable code.

**Never start a task branch from master:** it currently contains early scaffold instead of the implemented application.

Read before coding:
- AGENTS.md
- docs/MVP_ARCHITECTURE.md
- docs/DIAGNOSTICS_API_SPEC.md
- docs/TELEMETRY_ANALYTICS_RESPONSIBILITIES.md
- docs/DIAGNOSTICS_OFFLINE_IMPLEMENTATION_PLAN.md
- docs/TESTBED_INTELLIGENCE_VISION.md
- docs/DIAGNOSTICS_AGENT_README.md

## Immediate parallel work

| Agent | Branch suggestion | Handoff | Exclusive code ownership |
| --- | --- | --- | --- |
| A | feat/diag-stats-core | docs/handoffs/AGENT_A_STATS_CORE.md | New src/diagnostics/stats/**, tests/diagnostic_statistics_tests.cpp; CMakeLists.txt integration within A only |
| B | test/diag-scenario-lab | docs/handoffs/AGENT_B_SYNTHETIC_SCENARIOS.md | tools/diagnostic_scenarios/**, tests/fixtures/diagnostics/**; no root CMake changes |
| C | feat/diag-exact-sync-lifecycle | docs/handoffs/AGENT_C_EXACT_SYNC.md | src/diagnostics/ExactSyncDiagnostics.hpp, tests/exact_sync_diagnostics_tests.cpp; if another file needs editing, request coordination |

Work owned by the coordinator (this conversation): contract alignment, review, integration, later headless HTTP adapter D, incident episode/evidence work, and non-core telemetry color cleanup. Agents do not independently build incompatible duplicate HTTP, MCP or statistics exporters.

## Minimal shared contract v0 — freeze this before parallel changes

This is an **offline test interchange contract**; not yet the final HTTP/OpenAPI contract.

- Frame identity is **both** unsigned 64-bit values stream_epoch and frame_id. In JSON serialize them as decimal strings. Never match by frame_id alone or by approximate timestamp.
- Each synthetic observation must declare data_origin=synthetic; source host role (ubuntu-source, ubuntu-mediamtx, jetson, windows-reg), type, local clock domain, local monotonic timestamp as decimal-string nanoseconds, and process/session reset identity. UTF-8 JSON fixtures are valid. Example observation:

      {"schema_version":"diag-fixture/v1","data_origin":"synthetic","host_role":"windows-reg","session_id":"reg-session-1","clock_domain":"reg-steady","t_monotonic_ns":"16666667","type":"video.decoded","frame_key":{"stream_epoch":"12","frame_id":"101"},"attributes":{}}

- For paths without a FrameKey (e.g. process started), frame_key may be null; absence must never be synthesized as a zero-valued FrameKey.
- Numeric sample population, metric unit and session/reset domain are mandatory metadata in every **statistic**. Missing measurement != numerical zero.
- Event types are provisional and additive; keep scenario-specific values in attributes. Before changing this base envelope, propose a change in the coordinator chat.
- Counters reset only within their declared owner/session domain. Cross-host monotonic times cannot be subtracted.
- A generated scenario has manifest (scenario_id, generator version/seed, synthetic host clock specs, injected faults, expected assertions) plus an ordered event stream; deterministic replay uses virtual time and fixed seed.

A owns the reusable statistical ABI and may propose additions to metric definitions. B owns test fixture serialization, not production telemetry types. C owns Exact Sync diagnostic lifecycle events and may propose new observation types. Coordinator approves cross-boundary additions; no agent should silently freeze a private incompatible v1 format.

## Shared CI and quality criteria

- No RTSP server, Jetson, camera, display, Vulkan context or live network required for the offline acceptance tests.
- Windows/Linux compilation or Python unit tests where applicable.
- No synchronous HTTP/file writes or percentile sorting on decoder, rendering or CV hot paths.
- All successful matching remains strictly FrameKey equality. No software video-decoder fallback or CPU roundtrip.
- Cover invalid, missing, stale and ambiguous information, not just happy path.
- Test counters, duration boundaries and deterministic virtual time. Report algorithm complexity, memory bounds and missing-data semantics.
- The agent must provide PR link, exact branch/base/commit, file list, tests executed with results, unfinished items and integration dependencies.
- Report 'offline verified' only if actually tested; do not claim 'hardware validated'.

## Merge order and next wave

1. Documentation PR #33 (architecture and handoffs, no executable files).
2. Agent A metrics/stats library, after independent CI review.
3. Agent B fixture/test corpus; wire fixture consumption with A via a coordinator integration PR if needed.
4. Agent C Exact Sync lifecycle accounting, independently regression tested.
5. Coordinator integrates, resolves schema/CI overlaps and freezes v1 HTTP DTOs/OpenAPI based on proven statistics and Exact Sync.
6. Agent D API + headless fixture service (separate handoff after 1–5).
7. Episodes/evidence storage and MCP adapter only after read-only service contract is stable.

Tasks B/C can merge before A if their work is independent; the numbered list reflects easiest integration ordering, not a strict dependency.

## Review / handoff checklist for returning to coordinator

Every agent response/PR must answer:
- What exactly was implemented and where?
- How is it tested *without* Jetson/RTSP?
- What are its observability, source-clock and reset semantics?
- Does it preserve strict frame synchronization and real-time path safety?
- Any public contract/schema changes requiring agreement?
- Which CI tests passed and which are not run?
- What is the branch, commit, PR and safest integration sequence?

Do not send unbounded copied logs back to the coordinator; provide concise test summaries and links to CI artifacts.
