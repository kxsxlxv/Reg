#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace reg::platform {

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

private:
    static constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};

    std::uintptr_t handle_{kInvalidHandle};
#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::platform
