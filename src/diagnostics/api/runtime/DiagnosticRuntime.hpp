#pragma once

#include "diagnostics/adapter/TelemetryAdapter.hpp"
#include "diagnostics/api/codec/DiagnosticJsonApi.hpp"
#include "diagnostics/api/transport/LoopbackHttpServer.hpp"
#include "telemetry/TelemetryModel.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace reg::diagnostics::api::runtime {

// The only production bridge between Reg's owner-local TelemetryModel and the
// read-only loopback HTTP endpoint. No work is scheduled on a video/CV callback.
// The caller owns TelemetryModel; stop/destroy this object before that model.
class DiagnosticRuntime final {
public:
    DiagnosticRuntime(telemetry::TelemetryModel& source,
                      std::uint16_t port = 8765,
                      std::chrono::milliseconds samplePeriod =
                          std::chrono::seconds{1});
    ~DiagnosticRuntime();

    DiagnosticRuntime(const DiagnosticRuntime&) = delete;
    DiagnosticRuntime& operator=(const DiagnosticRuntime&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t boundPort() const noexcept;

    // Exposed for deterministic offline tests and useful if a future
    // coordinator uses an externally scheduled low-priority sampler.
    // Only one sampling owner/thread may call sampleOnce at a time.
    void sampleOnce(std::chrono::steady_clock::time_point now =
                        std::chrono::steady_clock::now());

    // Safe to invoke concurrently on HTTP worker threads. Returns 503 for
    // snapshot/status until at least one timed TelemetryModel sample exists.
    [[nodiscard]] HttpResponse respond(const HttpRequest& request) const;

private:
    void sampler(std::stop_token token) noexcept;

    telemetry::TelemetryModel& source_;
    std::chrono::milliseconds samplePeriod_;
    std::chrono::steady_clock::time_point startedAt_;
    const std::string sessionId_;
    adapter::ResetIdentityState resetIdentity_;

    mutable std::mutex cacheMutex_;
    std::shared_ptr<const Snapshot> cached_;
    std::uint64_t sequence_{};

    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    transport::LoopbackHttpServer server_;
    std::jthread samplerThread_;
};

} // namespace reg::diagnostics::api::runtime
