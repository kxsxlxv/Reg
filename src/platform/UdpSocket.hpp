#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace reg::platform {

struct UdpEndpoint {
    std::uint32_t ipv4{}; // IPv4 address in host byte order.
    std::uint16_t port{}; // UDP port in host byte order.
    bool operator==(const UdpEndpoint&) const = default;
};

class UdpSocket final {
public:
    UdpSocket(
        std::string_view bindAddress,
        std::uint16_t port,
        std::uint32_t receiveTimeoutMs = 100);
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Returns 0 on receive timeout. A zero-length UDP datagram is treated as no
    // useful application packet and is also returned as 0.
    std::size_t receive(std::span<std::uint8_t> buffer);

    // Endpoint-aware variant for CVV1 fragment identity. CVM1 keeps receive().
    std::size_t receiveFrom(std::span<std::uint8_t> buffer, UdpEndpoint& peer);

    std::uint16_t localPort() const;

private:
    static constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};

    std::uintptr_t handle_{kInvalidHandle};
#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::platform
