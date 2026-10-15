# Reg — Diagnostic API v1 runtime integration

## Enabling the service

The diagnostic API is **off by default**. Opt in on an existing Reg invocation:

```sh
reg_probe --url rtsp://127.0.0.1:8555/reg --enable-diagnostic-api --diagnostic-api-port 8765
```

Once running, request:

```sh
curl -i http://127.0.0.1:8765/healthz
curl -i http://127.0.0.1:8765/v1/status
curl -i 'http://127.0.0.1:8765/v1/snapshot?sections=rtsp,video,cv,exact_sync,recorder,ui'
curl -i http://127.0.0.1:8765/v1/capabilities
curl -i http://127.0.0.1:8765/openapi.json
```

HTTP is bound only to IPv4 **127.0.0.1**; no LAN listening, no authentication and no mutators. Local users/processes can access it. Requests require a matching loopback Host. Do not expose the port via a reverse proxy or forwarding without designing separate authentication. The HTTP transport does not set permissive CORS headers.

The service is disabled when no opt-in argument is present, including when only `--diagnostic-api-port` is specified. Port must be 1–65535. A bind failure reports a warning, disables the API and **does not terminate video rendering**.

## Data flow and ownership

`TelemetryModel` remains owned by the application. Its counters are updated from existing Reg-owned atomics, receiver statistics, exact-sync observer and recorder statistics. The update is performed even if the Telemetry window is disabled/unavailable **when the API is enabled**. Decoder/CV thread instrumentation is unchanged.

`DiagnosticRuntime` runs a **single low-priority observational sampling loop** (nominally 1 Hz), snapshots bounded `TelemetryModel` history off the decode/render callbacks, uses Agent A's `ResetIdentityState` across history eviction, and produces immutable `api::Snapshot` copies. HTTP workers consume only shared snapshots and pure Agent B codec; there is no per-request TelemetryModel lock, video frame copy or JSON serialization on hot paths. The worker returns 503 until the first real timed sample; `/healthz` is **listener liveness only** even when RTSP/CV are unavailable.

Publication timestamp is UTC, sampling windows and freshness use the **Reg-local steady clock** only. `data_origin=live` is set only from real `TelemetryModel` snapshots. External UNIGINE, MediaMTX and Jetson metrics are **not invented**; exact matched decisions must not be interpreted as physical scanout. `uint64` identities are decimal JSON strings. Rate windows and denominator semantics follow the original A adapter.

In cleanup, the HTTP listener is stopped and all workers joined before the sampler and its owning `TelemetryModel` can be destroyed. No asynchronous output or background operation survives a Reg session.

## Offline acceptance without RTSP/Jetson/GPU

The normal Windows and Linux CTest suites include:

- `diagnostic_telemetry_adapter_v1` — real headers, reset boundaries, FPS and missing values.
- `diagnostic_api_codec_v1` — read-only routes, OpenAPI, escaping and JSON values.
- `diagnostic_http_transport_v1` — actual loopback TCP clients and Host/framing checks.
- `diagnostic_runtime_v1` — actual `TelemetryModel` counters delivered to `/v1/snapshot` through the real TCP server, including opt-in, unavailable-before-first-sample and lifecycle tests.

The `Diagnostics headless` workflow also compiles and executes these tests with GCC independently of graphics/media dependencies. Hardware latency/throughput and UI behaviour require later tests on the actual UNIGINE/Jetson/Windows testbed.

## Intentional scope boundaries

Diagnostic API v1 remains **read-only and Reg-local**. This release does not introduce telemetry emitters on Jetson, MediaMTX or UNIGINE; MCP tools; rate-dependent adaptive playout; packet captures; cross-host latency subtraction; physical Vulkan present feedback; external web exposure; or a no-graphics daemon executable.
