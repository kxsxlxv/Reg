#pragma once

#include "diagnostics/api/DiagnosticContract.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace reg::diagnostics::api::transport {

using Handler = std::function<HttpResponse(const HttpRequest&)>;

// Off by default: constructing the object does not open any socket or thread.
// Only the IPv4 address 127.0.0.1 is supported; there is intentionally no
// configurable bind address and no remote access/authentication facility.
// The supplied Handler must be bounded, thread-safe, and must not call stop().
class LoopbackHttpServer final {
public:
    static constexpr std::size_t kMaxHeaderBytes = 8192;
    static constexpr std::size_t kMaxTargetBytes = 2048;
    static constexpr std::size_t kMaxResponseBodyBytes = 1024 * 1024;
    static constexpr std::size_t kWorkers = 2;

    // Port 0 requests an OS-assigned ephemeral port, useful for offline tests.
    explicit LoopbackHttpServer(Handler handler, std::uint16_t port = 8765);
    ~LoopbackHttpServer();

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    // Idempotent. start() throws std::runtime_error for bind/socket failure.
    // Lifecycle methods must be called by the owner, not from the Handler.
    void start();
    void stop() noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    // 0 before start and after stop, otherwise getsockname()'s actual port.
    [[nodiscard]] std::uint16_t boundPort() const noexcept;

private:
    static constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};
    void worker(std::size_t slot) noexcept;
    void handleClient(std::uintptr_t socket) noexcept;

    const Handler handler_;
    const std::uint16_t requestedPort_;
    mutable std::mutex lifecycleMutex_;
    std::mutex activeMutex_;
    std::array<std::uintptr_t, kWorkers> activeSockets_{kInvalidSocket, kInvalidSocket};
    std::array<std::thread, kWorkers> workers_{};
    std::uintptr_t listener_{kInvalidSocket};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> boundPort_{0};
#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::diagnostics::api::transport
