#pragma once

#include "diagnostics/api/DiagnosticContract.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace reg::diagnostics::api::codec {

inline constexpr std::size_t kMaxResponseBodyBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxRequestTargetBytes = 8192U;

// Pure, GET-only JSON router. Neither overload owns a socket, thread or telemetry source.
[[nodiscard]] HttpResponse respond(const HttpRequest& request, const Snapshot& snapshot);

// Explicit no-snapshot state: /v1/status and /v1/snapshot return 503. This
// overload avoids guessing availability from an empty but valid Snapshot.
[[nodiscard]] HttpResponse respond(const HttpRequest& request, const Snapshot* snapshot);

// Exposed for byte-level UTF-8/escaping regression tests. Invalid UTF-8 input
// is replaced by U+FFFD; all control bytes are escaped; no surrounding quotes.
[[nodiscard]] std::string escape_json(std::string_view value);

} // namespace reg::diagnostics::api::codec
