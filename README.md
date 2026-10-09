# Reg

**Reg is an NVIDIA/Vulkan-based, frame-accurate observation and diagnostic subsystem for a heterogeneous UAV simulation / semi-physical test stand.** The current development rig has three hosts: an Ubuntu computer running UNIGINE 2.22 SIM with an H.264/RTSP source and MediaMTX, a Jetson Orin Nano running computer vision and sending CVM1 frame metadata, and a Windows computer running Reg for Raw video, exact CV Overlay, monitoring and recording.

## Four project pillars

1. **Real-time visualization:** hardware-only H.264 decode via FFmpeg/Vulkan, newest-frame-only Raw, multi-window presentation and recorder.
2. **Exact correspondence:** the source-assigned H.264 SEI FrameKey (stream_epoch, frame_id) keys every decoded image and Jetson CV metadata packet; no approximate timestamp matches.
3. **Cross-host observability and reproducibility:** structured metrics, events, diagnostic histories, software/configuration provenance, and evidence-based, reproducible experiments.
4. **AI-assisted testbed intelligence:** future diagnostic agents use typed data and deterministic tests to formulate and verify fault hypotheses instead of relying on screenshots, unsourced log summaries, or unexamined model assertions.

**Pillars 3 and 4 are accepted foundational design goals.** This is not a claim that an HTTP Diagnostic API, MCP adapter, Jetson exporter, LLM analyzer or autonomous test runner is currently implemented. The design, risks and measurement methodology are documented in [Testbed Intelligence Vision](docs/TESTBED_INTELLIGENCE_VISION.md), [Diagnostic API specification](docs/DIAGNOSTICS_API_SPEC.md), and [agent guide](docs/DIAGNOSTICS_AGENT_README.md).

Reg is not an autonomous flight controller; "semi-physical" describes the current heterogeneous compute/video/CV testing configuration. Full airframe/autopilot hardware-in-the-loop validation is out of scope until actually integrated and independently tested.

## Rendering and performance contracts

- Ubuntu 26.04 (Wayland/X11) and Windows 10/11, Vulkan 1.3+, C++23, SDL3 and FFmpeg;
- NVIDIA H.264 hardware decoding, no software-decoder fallback, no OpenGL renderer or GPU->CPU->GPU decoded-video roundtrip in live presentation;
- Raw never accumulates a long FIFO; newest decoded frame wins;
- delayed CV Overlay accepts metadata only for the exact same source FrameKey, or drops the video frame at its deadline;
- continuous compressed MKV Blackbox is independent of display and must not block decode;
- loss of a diagnostic/MCP/LLM service must never compromise streaming, CV processing, presentation or recording.

Current components in active development branches include a GUI Launcher, multi-display Raw/Overlay/Telemetry, Blackbox/Replay, Exact Sync diagnostics and RTSP recovery mechanisms. Automated CI success is not equivalent to live three-computer hardware acceptance. Always verify the target feature's revision, build and acceptance documentation.

## Start here

- [MVP architecture and hard real-time invariants](docs/MVP_ARCHITECTURE.md)
- [Project pillar and experimental methodology: Testbed Intelligence](docs/TESTBED_INTELLIGENCE_VISION.md)
- [Reg Diagnostic API contract and metric catalog (proposed)](docs/DIAGNOSTICS_API_SPEC.md)
- [README for future Codex / diagnostic agents](docs/DIAGNOSTICS_AGENT_README.md)
- [Mandatory contributor and agent observability policy](AGENTS.md)
- [RTSP restart and recovery test matrix](docs/RESTART_RECOVERY_VALIDATION.md)
- [H.264 source FrameIdentity specification](docs/FRAME_IDENTITY_SEI.md)
- [Launcher usage, configuration and updates](docs/LAUNCHER.md)
- [Build notes](docs/BUILD.md)

## Development branch note

At the time of this update, GitHub's master branch contains only early repository scaffolding. Most of the implemented Reg application lives on active feature/integration branches. Confirm the intended base branch before building or merging changes. This documentation PR targets feat/netimgui-russian-detr. Source code is unchanged by this design update.
