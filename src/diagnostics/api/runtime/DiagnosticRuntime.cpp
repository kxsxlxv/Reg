#include "diagnostics/api/runtime/DiagnosticRuntime.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace reg::diagnostics::api::runtime {
namespace {

std::string utcNow() {
    // C++ chrono civil calendar avoids std::gmtime's global shared state.
    using namespace std::chrono;
    const auto instant = floor<seconds>(system_clock::now());
    const auto civil = floor<days>(instant);
    const year_month_day ymd{civil};
    const hh_mm_ss time{instant - civil};
    char buffer[32]{};
    (void)std::snprintf(buffer, sizeof(buffer),
                        "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
                        static_cast<int>(ymd.year()),
                        static_cast<unsigned>(ymd.month()),
                        static_cast<unsigned>(ymd.day()),
                        static_cast<long long>(time.hours().count()),
                        static_cast<long long>(time.minutes().count()),
                        static_cast<long long>(time.seconds().count()));
    return buffer;
}

std::string newSessionId() {
    // This is an opaque per-process-session discriminator, not a secret or a
    // host clock for latency calculations. No raw hostname/user ID is exposed.
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    return "reg.session:" +
        std::to_string(static_cast<std::uint64_t>(stamp));
}

} // namespace

DiagnosticRuntime::DiagnosticRuntime(telemetry::TelemetryModel& source,
                                     std::uint16_t port,
                                     std::chrono::milliseconds samplePeriod)
    : source_(source),
      samplePeriod_(samplePeriod),
      startedAt_(std::chrono::steady_clock::now()),
      sessionId_(newSessionId()),
      server_([this](const HttpRequest& request) { return respond(request); }, port) {
    if (samplePeriod_ <= std::chrono::milliseconds{0}) {
        throw std::invalid_argument("Diagnostic sample period must be positive");
    }
}

DiagnosticRuntime::~DiagnosticRuntime() { stop(); }

void DiagnosticRuntime::start() {
    if (server_.isRunning()) return;
    // No listener or sampler exists until the explicit start() call. Fail
    // closed: if socket bind or thread creation fails, do not leave a server.
    server_.start();
    try {
        samplerThread_ = std::jthread([this](std::stop_token token) {
            sampler(token);
        });
    } catch (...) {
        server_.stop();
        throw;
    }
}

void DiagnosticRuntime::stop() noexcept {
    // First withdraw the public entrypoint; HTTP Handler captures 'this'.
    // Then terminate the sampler before TelemetryModel can be destroyed.
    server_.stop();
    samplerThread_.request_stop();
    wake_.notify_all();
    if (samplerThread_.joinable()) samplerThread_.join();
}

bool DiagnosticRuntime::running() const noexcept { return server_.isRunning(); }
std::uint16_t DiagnosticRuntime::boundPort() const noexcept { return server_.boundPort(); }

void DiagnosticRuntime::sampleOnce(std::chrono::steady_clock::time_point now) {
    // Copy at most TelemetryModel's bounded retention off critical threads.
    // Never read receiver-owned clocks or invent external-host measurements.
    const auto local = source_.snapshot();
    if (local.samples.empty()) return; // not a real observation yet

    adapter::Context context{};
#ifdef _WIN32
    context.hostId = "reg-windows";
#else
    context.hostId = "reg-linux";
#endif
    context.processSessionId = sessionId_;
    context.observedAtUtc = utcNow();
    context.dataOrigin = "live";
    context.now = now;
    context.freshness = std::chrono::seconds{2};
    context.monotonicUptimeMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now >= startedAt_ ? now - startedAt_ :
                               std::chrono::steady_clock::duration::zero()).count());
    context.resetIdentityState = &resetIdentity_;
    context.sequence = ++sequence_;
    auto next = std::make_shared<const Snapshot>(adapter::makeSnapshot(local, context));
    std::lock_guard lock(cacheMutex_);
    cached_ = std::move(next);
}

HttpResponse DiagnosticRuntime::respond(const HttpRequest& request) const {
    // /healthz remains listener liveness only even if the camera or CV fails.
    // The null pointer overload produces explicit 503 until sampled once.
    std::shared_ptr<const Snapshot> local;
    {
        std::lock_guard lock(cacheMutex_);
        local = cached_;
    }
    return codec::respond(request, local.get());
}

void DiagnosticRuntime::sampler(std::stop_token token) noexcept {
    while (!token.stop_requested()) {
        try {
            sampleOnce();
        } catch (...) {
            // Diagnostics are strictly best effort; no exceptions escape onto
            // render or decode threads and no credentials enter a log.
        }
        std::unique_lock lock(wakeMutex_);
        wake_.wait_for(lock, token, samplePeriod_, [] { return false; });
    }
}

} // namespace reg::diagnostics::api::runtime
