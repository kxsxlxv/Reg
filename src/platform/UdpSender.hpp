#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace reg::platform {

class UdpSender final {
public:
    UdpSender(
        std::string_view remoteAddress,
        std::uint16_t remotePort);
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Non-blocking send. Returns false only when the OS transmit buffer would
    // block. Other socket errors are reported as exceptions.
    bool trySend(
        std::span<const std::uint8_t> datagram);

private:
    static constexpr std::uintptr_t kInvalidHandle =
        ~std::uintptr_t{0};

    std::uintptr_t handle_{kInvalidHandle};

#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::platform
