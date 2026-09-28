#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace reg::platform {

class UdpSocket final {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) = delete;
    UdpSocket& operator=(UdpSocket&&) = delete;

    void bindIpv4(std::string_view address, std::uint16_t port);

    std::optional<std::size_t> receive(
        std::span<std::uint8_t> buffer,
        std::chrono::milliseconds timeout);

private:
    std::intptr_t socket_{-1};
#ifdef _WIN32
    bool winsockInitialized_{false};
#endif
};

} // namespace reg::platform
