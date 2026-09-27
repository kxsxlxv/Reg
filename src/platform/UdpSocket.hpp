#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace reg::platform {

class UdpSocket final {
public:
    UdpSocket(
        const std::string& bindAddress,
        std::uint16_t port,
        std::chrono::milliseconds receiveTimeout = std::chrono::milliseconds(100),
        int receiveBufferBytes = 1 << 20);
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    std::optional<std::size_t> receive(std::span<std::uint8_t> buffer);

    std::uint16_t localPort() const noexcept { return localPort_; }

private:
    void close() noexcept;

    std::uintptr_t socket_{~std::uintptr_t{0}};
    std::uint16_t localPort_{0};
#ifdef _WIN32
    bool winsockStarted_{false};
#endif
};

} // namespace reg::platform
