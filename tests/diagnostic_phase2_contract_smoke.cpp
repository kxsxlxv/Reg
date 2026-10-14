// Coordinator review-only cross-agent acceptance probe.
// Uses REAL Reg headers and implementations, not mirror DTOs.
// Deliberately detects loss of A's reset identity in B's JSON output.
#include "diagnostics/adapter/TelemetryAdapter.hpp"
#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    using namespace reg;
    using namespace std::chrono_literals;
    telemetry::Snapshot input;
    telemetry::Counters c{};
    c.decoderSessions = 1;
    c.exactSync.sessionGeneration = 7;
    c.decodedFrames = 2;
    c.rawPresentedFrames = 2;
    c.rtspConnected = true;
    c.videoSignalPresent = true;
    input.samples.push_back({std::chrono::steady_clock::time_point{1s}, c});
    input.counters = c;

    diagnostics::adapter::Context ctx{};
    ctx.hostId = "reg-windows";
    ctx.processSessionId = "p-1";
    ctx.observedAtUtc = "2026-10-13T18:00:00Z";
    ctx.now = std::chrono::steady_clock::time_point{1s};
    ctx.sequence = 1;
    const auto snapshot = diagnostics::adapter::makeSnapshot(input, ctx);
    const auto response = diagnostics::api::codec::respond({"GET", "/v1/snapshot"}, snapshot);
    if (response.statusCode != 200) {
        std::cerr << "phase2 contract smoke: unexpected HTTP status\n";
        return 1;
    }
    const auto& body = response.body;
    if (body.find("\"name\":\"decoder.decoded_frames_total\"") == std::string::npos ||
        body.find("\"counter\":\"2\"") == std::string::npos) {
        std::cerr << "phase2 contract smoke: decoded counter lost\n";
        return 1;
    }
    if (body.find("\"reset_id\":null") != std::string::npos) {
        std::cerr << "phase2 contract smoke: A/B incompatible reset_id (serialized to null)\n";
        return 1;
    }
    if (body.find("\"name\":\"exact_sync.matched_pairs_total\"") == std::string::npos) {
        std::cerr << "phase2 contract smoke: ExactSync absent\n";
        return 1;
    }
    std::cout << "phase2 contract smoke: PASS\n";
}
