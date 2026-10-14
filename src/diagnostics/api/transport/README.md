# Reg Diagnostic HTTP transport (Agent C)

This library transports the Phase-2 **read-only** Diagnostic API v1 on a
hard-coded IPv4 `127.0.0.1` listener. It knows only the shared
`api::HttpRequest` and `api::HttpResponse` DTOs. It does not collect telemetry,
read files, issue commands, log request contents, or serialize JSON. No socket
exists until the owner explicitly calls `start()`.

## Integration (coordinator-owned)

```cpp
#include "diagnostics/api/transport/LoopbackHttpServer.hpp"

using reg::diagnostics::api::HttpRequest;
using reg::diagnostics::api::HttpResponse;
using reg::diagnostics::api::transport::LoopbackHttpServer;

LoopbackHttpServer server(
    [&](const HttpRequest& request) -> HttpResponse {
        return readOnlyApiHandler(request); // supplied by Agent B + coordinator
    }, 8765); // or 0 for an OS-assigned port during tests

// Only after explicit operator opt-in:
server.start();
const auto actualPort = server.boundPort();
// On shutdown, outside of the handler:
server.stop();
```

The owner **must** stop and destroy the server before destroying any objects
captured by the handler. `start()` and `stop()` are idempotent; `start()` throws
on bind/listen/socket/thread creation failures, including an occupied port.
`boundPort()` returns zero when stopped. Lifecycle calls are serialized, but
`stop()` must never be called by the handler itself: it joins its own worker
threads. The supplied handler must be thread-safe, bounded, non-blocking and
avoid decode/render/metadata callbacks and heavyweight work. An arbitrary
blocking handler cannot safely be cancelled by the transport. HTTP/network
work runs on two fixed workers, not the render/decode/UI thread.

A self-contained Linux test build, **without full Reg dependencies**:

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -pthread -Isrc src/diagnostics/api/transport/LoopbackHttpServer.cpp \
  tests/diagnostic_http_transport_tests.cpp -o diagnostic_http_transport_tests
./diagnostic_http_transport_tests
```

Coordinator CMake needs to compile/link `LoopbackHttpServer.cpp` and link
**`ws2_32` on Windows** (`target_link_libraries(... PRIVATE ws2_32)`;
`WinSock2.h` precedes any `Windows.h`). Linux uses libc sockets and the normal
C++ threading toolchain (e.g. `Threads::Threads` when required). CI target and
production integration are deliberately outside Agent C ownership.

## Protocol and limits

- Only `GET <origin-form-target> HTTP/1.1`; no request bodies, streaming,
  chunking, HTTP/2, TLS, keepalive, upgrade, arbitrary methods or file access.
- Port configured by constructor, never IP address: binding is strictly
  `127.0.0.1` and unexpected non-loopback peers are rejected.
- Request headers <= 8192 bytes including `

`, target <= 2048 bytes,
  read deadline 1.9 s with short socket timeouts; malformed headers are 400,
  long URI 414, oversized header 431 and incomplete/idle requests 408.
- HTTP/1.1 `Host` is allowlisted as **only `127.0.0.1` or `localhost`**
  (ASCII case-insensitive for `localhost`), optionally followed by a decimal
  `:<actual_bound_port>` matching the value from `getsockname()` — including
  when the configured port was 0. External DNS names, userinfo, IPv6 literals,
  malformed/alternate ports and duplicate `Host` headers receive 400 before
  any Handler invocation. This prevents ordinary Host-based DNS rebinding from
  treating a third-party domain as this loopback API.
- To eliminate request-smuggling framing ambiguities, **all**
  `Content-Length` and `Transfer-Encoding` headers are rejected (including
  `Content-Length: 0`). Conflicting/repeated `Host`, obsolete folded headers,
  whitespace before header colons and non-CRLF line endings are rejected.
- Maximum response body 1,048,576 bytes; larger/invalid responses and handler
  exceptions become sanitized 500 errors. Only JSON/plain-text Content-Type
  values are accepted; a fixed status phrase prevents HTTP header injection.
- Every response has exact byte `Content-Length`, `Connection: close`,
  `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`.
- Two fixed workers and a small listen backlog bound active socket processing;
  shutdown interrupts blocked socket I/O. Individual handlers must still
  execute promptly. This is not a general-purpose HTTP server.

## Security boundary

Loopback is **not authentication**. An explicit Host allowlist mitigates
Host-based DNS rebinding, but it does not authenticate a local process or provide
an authorization boundary. **CORS is not enabled**: the server never emits
`Access-Control-Allow-Origin`, especially not `*`; coordinator must not add
permissive CORS response headers. Any future browser UI, non-loopback support
or proxying requires a separate origin/authentication threat-model review.
Other local applications/users, browser
pages through requests to localhost, and local malware may be able to call an
opted-in API. Diagnostics data must therefore be treated as accessible to
local callers, with all payload contents sanitized by the coordinator/Agent B.
This transport offers **no** LAN mode, credentials or privilege separation.
Do not place tokens, raw RTSP credentials, arbitrary log text, local paths or
host secrets in Handler responses. No security claim is made for exposing the
service through forwarding proxies or tunnelling tools. The API must be kept
disabled until explicitly enabled by the operator.

### Observability impact

The transport does not add domain metrics or change RTSP, Vulkan, CVM1, Exact
Sync or UI state. Listener state, bound port and start errors are observable
locally via the return/exception surface; exposing them as API metrics is a
coordinator-owned integration decision. No new per-frame instrumentation or
additional locks on the live video path are introduced.
