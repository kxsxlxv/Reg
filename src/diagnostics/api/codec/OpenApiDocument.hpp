#pragma once

#include <string_view>

namespace reg::diagnostics::api::codec {

// OpenAPI 3.1: kept adjacent to the implementation. The regression tests
// validate this document and actual responses independently of HTTP transport.
[[nodiscard]] constexpr std::string_view openapi_document() {
    return R"OPENAPI({
  "openapi": "3.1.0",
  "info": {"title": "Reg read-only Diagnostic API", "version": "1.0.0", "description": "Loopback-only, GET-only. Unmatched paths respond 404/not_found. Liveness is not RTSP/video/CV health. Numerical identities and counters are decimal strings. A missing observation is null, never zero."},
  "servers": [{"url": "http://127.0.0.1:8765", "description": "Illustrative default; listener opt-in and port configurable by host"}],
  "paths": {
    "/healthz": {"get": {
      "operationId": "getHealthz", "description": "Service liveness ONLY. Does not assert stream, decoder, CV or physical display health.",
      "responses": {"200": {"description": "Listener responding", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Health"}}}}, "400": {"$ref": "#/components/responses/BadQuery"}, "405": {"$ref": "#/components/responses/MethodNotAllowed"}, "413": {"$ref": "#/components/responses/RequestTooLarge"}}
    }},
    "/v1/capabilities": {"get": {
      "operationId": "getCapabilities", "description": "Supported API routes and each section's current availability; unavailable is not disabled or healthy.",
      "responses": {"200": {"description": "Capabilities", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Capabilities"}}}}, "400": {"$ref": "#/components/responses/BadQuery"}, "405": {"$ref": "#/components/responses/MethodNotAllowed"}, "413": {"$ref": "#/components/responses/RequestTooLarge"}}
    }},
    "/v1/status": {"get": {
      "operationId": "getStatus", "description": "Compact status and freshness per section; no health inferred from listener liveness.",
      "responses": {"200": {"description": "Compact snapshot", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/StatusSnapshot"}}}}, "400": {"$ref": "#/components/responses/BadQuery"}, "405": {"$ref": "#/components/responses/MethodNotAllowed"}, "413": {"$ref": "#/components/responses/RequestTooLarge"}, "503": {"$ref": "#/components/responses/SnapshotUnavailable"}}
    }},
    "/v1/snapshot": {"get": {
      "operationId": "getSnapshot", "description": "Bounded typed telemetry snapshot, maximum JSON body 1048576 bytes. Section names are allowlisted; absent sections are unavailable.",
      "parameters": [{"name": "sections", "in": "query", "required": false, "description": "Optional non-empty comma-separated unique names from rtsp,video,cv,exact_sync,recorder,ui. Duplicate/unknown/encoded/other parameters result in 400; omitted means all sections.", "schema": {"type": "string", "pattern": "^(rtsp|video|cv|exact_sync|recorder|ui)(,(rtsp|video|cv|exact_sync|recorder|ui))*$"}}],
      "responses": {"200": {"description": "Typed snapshot", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/DiagnosticSnapshot"}}}}, "400": {"$ref": "#/components/responses/BadQuery"}, "405": {"$ref": "#/components/responses/MethodNotAllowed"}, "413": {"$ref": "#/components/responses/RequestTooLarge"}, "503": {"$ref": "#/components/responses/SnapshotUnavailable"}}
    }},
    "/openapi.json": {"get": {
      "operationId": "getOpenApi", "description": "Self-contained OpenAPI 3.1 definition.",
      "responses": {"200": {"description": "OpenAPI document", "content": {"application/json": {"schema": {"type": "object", "required": ["openapi", "info", "paths"]}}}}, "400": {"$ref": "#/components/responses/BadQuery"}, "405": {"$ref": "#/components/responses/MethodNotAllowed"}, "413": {"$ref": "#/components/responses/RequestTooLarge"}}
    }}
  },
  "components": {
    "schemas": {
      "UInt64String": {"type": "string", "pattern": "^[0-9]+$", "description": "Lossless decimal-encoded uint64, including uint64_max; never a JSON number"},
      "Int64String": {"type": "string", "pattern": "^-?[0-9]+$", "description": "Lossless signed nanosecond window boundary in the owner's clock domain"},
      "NullableUInt64": {"oneOf": [{"$ref": "#/components/schemas/UInt64String"}, {"type": "null"}]},
      "NullableInt64": {"oneOf": [{"$ref": "#/components/schemas/Int64String"}, {"type": "null"}]},
      "NullableFiniteNumber": {"type": ["number", "null"], "description": "Non-finite doubles are encoded as null"},
      "NullableBoolean": {"type": ["boolean", "null"]},
      "NullableString": {"type": ["string", "null"], "description": "Identifier labels are restricted; runtime free-text and secrets are not reflected"},
      "CanonicalResetId": {"type": ["string", "null"], "pattern": "^[A-Za-z0-9_.:-]{1,128}$", "description": "Case-sensitive 1..128 byte ASCII reset identity scoped to its population, session and generation. Example: p-1.decoder:2 or p-1.exact:7. No slash/backslash, filesystem paths, URLs, credentials or arbitrary logs; invalid/absent values become null. A null reset ID is not comparable across snapshots."},
      "Availability": {"type": "string", "enum": ["fresh", "stale", "unavailable", "unsupported", "disabled", "error"]},
      "Provenance": {"type": "string", "enum": ["measured", "derived", "external", "estimated"]},
      "MetricKind": {"type": "string", "enum": ["counter", "gauge", "rate", "distribution"]},
      "FrameKey": {"type": "object", "additionalProperties": false, "required": ["stream_epoch", "frame_id"], "properties": {"stream_epoch": {"$ref": "#/components/schemas/UInt64String"}, "frame_id": {"$ref": "#/components/schemas/UInt64String"}}},
      "Status": {"type": "object", "additionalProperties": false, "required": ["name", "availability", "boolean_value", "text_value", "unsigned_value"], "properties": {
        "name": {"$ref": "#/components/schemas/NullableString"}, "availability": {"$ref": "#/components/schemas/Availability"},
        "boolean_value": {"$ref": "#/components/schemas/NullableBoolean"}, "text_value": {"$ref": "#/components/schemas/NullableString"}, "unsigned_value": {"$ref": "#/components/schemas/NullableUInt64"}
      }},
      "Metric": {"type": "object", "additionalProperties": false,
        "required": ["name", "availability", "unit", "owner", "population", "clock_domain", "reset_scope", "reset_id", "kind", "provenance", "counter", "value", "sample_count", "eligible_count", "missing_count", "timeout_count", "skipped_count", "window_start_ns", "window_end_ns", "window_complete", "min", "max", "mean", "p50", "p95", "p99", "quantile_relative_upper_error_bound", "quantiles_are_upper_bounds"],
        "properties": {
          "name": {"$ref": "#/components/schemas/NullableString"}, "availability": {"$ref": "#/components/schemas/Availability"},
          "unit": {"$ref": "#/components/schemas/NullableString"}, "owner": {"$ref": "#/components/schemas/NullableString"},
          "population": {"$ref": "#/components/schemas/NullableString"}, "clock_domain": {"$ref": "#/components/schemas/NullableString"},
          "reset_scope": {"$ref": "#/components/schemas/NullableString"}, "reset_id": {"$ref": "#/components/schemas/CanonicalResetId"},
          "kind": {"$ref": "#/components/schemas/MetricKind"}, "provenance": {"$ref": "#/components/schemas/Provenance"},
          "counter": {"$ref": "#/components/schemas/NullableUInt64"}, "value": {"$ref": "#/components/schemas/NullableFiniteNumber"},
          "sample_count": {"$ref": "#/components/schemas/NullableUInt64"}, "eligible_count": {"$ref": "#/components/schemas/NullableUInt64"},
          "missing_count": {"$ref": "#/components/schemas/NullableUInt64"}, "timeout_count": {"$ref": "#/components/schemas/NullableUInt64"},
          "skipped_count": {"$ref": "#/components/schemas/NullableUInt64"}, "window_start_ns": {"$ref": "#/components/schemas/NullableInt64"},
          "window_end_ns": {"$ref": "#/components/schemas/NullableInt64"}, "window_complete": {"$ref": "#/components/schemas/NullableBoolean"},
          "min": {"$ref": "#/components/schemas/NullableFiniteNumber"}, "max": {"$ref": "#/components/schemas/NullableFiniteNumber"},
          "mean": {"$ref": "#/components/schemas/NullableFiniteNumber"}, "p50": {"$ref": "#/components/schemas/NullableFiniteNumber"},
          "p95": {"$ref": "#/components/schemas/NullableFiniteNumber"}, "p99": {"$ref": "#/components/schemas/NullableFiniteNumber"},
          "quantile_relative_upper_error_bound": {"$ref": "#/components/schemas/NullableFiniteNumber"},
          "quantiles_are_upper_bounds": {"type": "boolean"}
        }
      },
      "Section": {"type": "object", "additionalProperties": false, "required": ["availability", "data_age_ms", "last_frame_key", "metrics", "statuses"], "properties": {
        "availability": {"$ref": "#/components/schemas/Availability"},
        "data_age_ms": {"$ref": "#/components/schemas/NullableUInt64"},
        "last_frame_key": {"oneOf": [{"$ref": "#/components/schemas/FrameKey"}, {"type": "null"}]},
        "metrics": {"type": "array", "items": {"$ref": "#/components/schemas/Metric"}},
        "statuses": {"type": "array", "items": {"$ref": "#/components/schemas/Status"}}
      }},
      "StatusSection": {"type": "object", "additionalProperties": false, "required": ["availability", "data_age_ms", "last_frame_key", "statuses"], "properties": {
        "availability": {"$ref": "#/components/schemas/Availability"}, "data_age_ms": {"$ref": "#/components/schemas/NullableUInt64"},
        "last_frame_key": {"oneOf": [{"$ref": "#/components/schemas/FrameKey"}, {"type": "null"}]},
        "statuses": {"type": "array", "items": {"$ref": "#/components/schemas/Status"}}
      }},
      "BaseEnvelope": {"type": "object", "required": ["schema_version", "host_role", "host_id", "process_session_id", "test_run_id", "data_origin", "observed_at_utc", "snapshot_sequence", "monotonic_uptime_ms", "sections"], "properties": {
        "schema_version": {"$ref": "#/components/schemas/NullableString"}, "host_role": {"$ref": "#/components/schemas/NullableString"},
        "host_id": {"$ref": "#/components/schemas/NullableString"}, "process_session_id": {"$ref": "#/components/schemas/NullableString"},
        "test_run_id": {"$ref": "#/components/schemas/NullableString"},
        "data_origin": {"type": ["string", "null"], "enum": ["live", "synthetic", null]},
        "observed_at_utc": {"$ref": "#/components/schemas/NullableString"},
        "snapshot_sequence": {"$ref": "#/components/schemas/UInt64String"}, "monotonic_uptime_ms": {"$ref": "#/components/schemas/NullableUInt64"}
      }},
      "SectionMap": {"type": "object", "additionalProperties": false, "properties": {
        "rtsp": {"$ref": "#/components/schemas/Section"}, "video": {"$ref": "#/components/schemas/Section"},
        "cv": {"$ref": "#/components/schemas/Section"}, "exact_sync": {"$ref": "#/components/schemas/Section"},
        "recorder": {"$ref": "#/components/schemas/Section"}, "ui": {"$ref": "#/components/schemas/Section"}
      }},
      "StatusSectionMap": {"type": "object", "additionalProperties": false, "required": ["rtsp", "video", "cv", "exact_sync", "recorder", "ui"], "properties": {
        "rtsp": {"$ref": "#/components/schemas/StatusSection"}, "video": {"$ref": "#/components/schemas/StatusSection"},
        "cv": {"$ref": "#/components/schemas/StatusSection"}, "exact_sync": {"$ref": "#/components/schemas/StatusSection"},
        "recorder": {"$ref": "#/components/schemas/StatusSection"}, "ui": {"$ref": "#/components/schemas/StatusSection"}
      }},
      "DiagnosticSnapshot": {"allOf": [{"$ref": "#/components/schemas/BaseEnvelope"}, {"type": "object", "properties": {"sections": {"$ref": "#/components/schemas/SectionMap"}}}]},
      "StatusSnapshot": {"allOf": [{"$ref": "#/components/schemas/BaseEnvelope"}, {"type": "object", "properties": {"sections": {"$ref": "#/components/schemas/StatusSectionMap"}}}]},
      "Health": {"type": "object", "additionalProperties": false, "required": ["service", "alive", "scope"], "properties": {
        "service": {"type": "string", "const": "reg-diagnostics"}, "alive": {"type": "boolean", "const": true},
        "scope": {"type": "string", "const": "listener_only"}
      }},
      "Capabilities": {"type": "object", "additionalProperties": false, "required": ["api_version", "host_role", "data_origin", "endpoints", "sections"], "properties": {
        "api_version": {"type": "string", "const": "1.0"}, "host_role": {"$ref": "#/components/schemas/NullableString"},
        "data_origin": {"type": ["string", "null"], "enum": ["live", "synthetic", null]},
        "endpoints": {"type": "array", "items": {"type": "string"}},
        "sections": {"type": "object", "additionalProperties": false, "required": ["rtsp", "video", "cv", "exact_sync", "recorder", "ui"], "properties": {
          "rtsp": {"$ref": "#/components/schemas/Availability"}, "video": {"$ref": "#/components/schemas/Availability"},
          "cv": {"$ref": "#/components/schemas/Availability"}, "exact_sync": {"$ref": "#/components/schemas/Availability"},
          "recorder": {"$ref": "#/components/schemas/Availability"}, "ui": {"$ref": "#/components/schemas/Availability"}
        }}
      }},
      "Error": {"type": "object", "additionalProperties": false, "required": ["error"], "properties": {"error": {"type": "object", "additionalProperties": false, "required": ["code", "message"], "properties": {"code": {"type": "string"}, "message": {"type": "string"}}}}}
    },
    "responses": {
      "NotFound": {"description": "404 unmatched route", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "BadQuery": {"description": "400 invalid, unsupported or repeated query parameter", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "MethodNotAllowed": {"description": "405; GET only", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "RequestTooLarge": {"description": "413 request target >8192 bytes or response >1048576 bytes", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "SnapshotUnavailable": {"description": "503 coordinator has no sampled snapshot to serve", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}}
    }
  }
})OPENAPI";
}

} // namespace reg::diagnostics::api::codec
