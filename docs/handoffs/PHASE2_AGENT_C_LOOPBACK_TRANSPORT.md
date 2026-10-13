# Phase-2 Agent C — Bounded Cross-Platform Loopback HTTP Transport

**Mission:** implement an isolated, opt-in, read-only HTTP/1.1 socket transport for Reg Diagnostic API v1, no telemetry, JSON or application wiring.
**Suggested branch:** `feat/diag-v1-loopback-http` from `feat/netimgui-russian-detr` after Phase-2 contract PR. Separate worktree.
**Handoff index:** `docs/handoffs/PHASE2_README.md`.

## Shared minimal entrypoint

Read `src/diagnostics/api/DiagnosticContract.hpp`. Provide:

    namespace reg::diagnostics::api::transport {
      using Handler = std::function<HttpResponse(const HttpRequest&)>;
      class LoopbackHttpServer; // configurable port / start / stop / bound port
    }

Exact constructor API is yours; document it and prefer composable RAII. The server only forwards bounded `HttpRequest` to a provided handler (Agent B + coordinator later) and writes `HttpResponse`. **No** direct `TelemetryModel`, `Statistics.hpp`, JSON rendering or test fixture loading. No production app hook.

## Hard requirements

- Bind only IPv4 `127.0.0.1`; never automatically bind to all interfaces, even if localhost address resolution returns an unexpected address. Default production listener disabled, opt-in wiring belongs to coordinator.
- Windows MSYS2 UCRT64: WinSock2 `WSAStartup` lifetime and `ws2_32` linker contract. Linux: POSIX sockets. C++23, avoid third-party HTTP dependencies. Test with port 0 ephemeral for CI; expose bound port to test.
- Bounded request: 8 KiB max headers, reject request body (GET-only API, but the network layer must reject or safely discard other methods/content), reject conflicting Content-Length / Transfer-Encoding and header smuggling, max target length (e.g., 2 KiB), invalid line endings and malformed HTTP method/target.
- Bounded response: hard cap <=1 MiB, stable Content-Length, Connection: close, no chunked-stream or keepalive work. Sanitize/whitelist response header content-type.
- Accept and read deadlines <=2 seconds; bounded connection concurrency <=2 (or single worker with timeout and backlog bound). Idle client must not stall application shutdown. `stop()` and destructor idempotent, deterministic and bounded even during an incomplete header read.
- Reject non-loopback peers if your socket platform exposes unexpected addresses. No unintended remote access or persistence. Do not expose arbitrary file, control commands or raw logs.
- No render/capture/CV pipeline callbacks; handler invoked on server worker(s) only. If handler throws, return redacted 500 without backtrace or secrets.
- A request from any local user may be able to reach this endpoint in v1. Clearly document the opt-in security boundary; no claim of authenticated remote access. Do not enable public bind for testing.

## OWN paths

`src/diagnostics/api/transport/**`, `tests/diagnostic_http_transport_tests.cpp`. Do NOT edit root CMake, CI workflows, shared DTO contract, codecs, telemetry adapter, application CLI or `src/main.cpp`. Provide exact Windows `ws2_32` linkage requirement for coordinator CMake.

## Acceptance tests

No Jetson or RTSP required:
1. Bind port 0 on 127.0.0.1; curl/raw TCP client GET /healthz routes to in-process stub; response Content-Length correct.
2. Non-GET, malformed headers, 8-KiB+ request, oversized path, partial write, conflicting body framing, client disconnect.
3. Client timeout and stop/destructor under blocked/incomplete request — no indefinite join.
4. Overflow response -> explicit bounded error; exception in handler -> sanitized error.
5. Multiple serial/concurrent requests cannot starve the process; listener does not consume renderer/UI thread.
6. Verify 0.0.0.0 and non-loopback bind cannot be configured (or rejected), and no publicly accessible listener is created.
7. Windows/Linux builds with actual sockets and no media/GPU/network-remote test resources.

Deliver separate PR with code, tests, benchmarks of request bound/shutdown and precise API signature for coordinator integration. Do not self-merge.
