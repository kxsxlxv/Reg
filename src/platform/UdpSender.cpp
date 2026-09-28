#include "platform/UdpSender.hpp"

#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace reg::platform {
namespace {

std::runtime_error socketError(
    const char* operation) {
#ifdef _WIN32
    return std::runtime_error(
        std::string(operation) +
        " failed with WSA error " +
        std::to_string(WSAGetLastError()));
#else
    return std::runtime_error(
        std::string(operation) +
        " failed with errno " +
        std::to_string(errno));
#endif
}

} // namespace

UdpSender::UdpSender(
    std::string_view remoteAddress,
    std::uint16_t remotePort) {
    if (remoteAddress.empty()) {
        throw std::invalid_argument(
            "UDP remote address must not be empty");
    }
    if (remotePort == 0) {
        throw std::invalid_argument(
            "UDP remote port must be in [1,65535]");
    }

#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw socketError("WSAStartup");
    }
    winsockInitialized_ = true;

    const SOCKET socketHandle =
        ::socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP);

    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        winsockInitialized_ = false;
        throw socketError(
            "socket(AF_INET, SOCK_DGRAM)");
    }

    handle_ =
        static_cast<std::uintptr_t>(
            socketHandle);
#else
    const int socketHandle =
        ::socket(AF_INET, SOCK_DGRAM, 0);

    if (socketHandle < 0) {
        throw socketError(
            "socket(AF_INET, SOCK_DGRAM)");
    }

    handle_ =
        static_cast<std::uintptr_t>(
            socketHandle);
#endif

    try {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(remotePort);

        const std::string addressText(
            remoteAddress);

        const int parsed =
            inet_pton(
                AF_INET,
                addressText.c_str(),
                &address.sin_addr);

        if (parsed != 1) {
            throw std::invalid_argument(
                "UDP remote address must be an IPv4 address");
        }

#ifdef _WIN32
        if (::connect(
                static_cast<SOCKET>(handle_),
                reinterpret_cast<
                    const sockaddr*>(&address),
                sizeof(address)) ==
            SOCKET_ERROR) {
            throw socketError("connect(UDP)");
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(
                static_cast<SOCKET>(handle_),
                FIONBIO,
                &nonBlocking) ==
            SOCKET_ERROR) {
            throw socketError(
                "ioctlsocket(FIONBIO)");
        }
#else
        if (::connect(
                static_cast<int>(handle_),
                reinterpret_cast<
                    const sockaddr*>(&address),
                sizeof(address)) != 0) {
            throw socketError("connect(UDP)");
        }

        const int currentFlags =
            fcntl(
                static_cast<int>(handle_),
                F_GETFL,
                0);

        if (currentFlags < 0) {
            throw socketError(
                "fcntl(F_GETFL)");
        }

        if (fcntl(
                static_cast<int>(handle_),
                F_SETFL,
                currentFlags | O_NONBLOCK) != 0) {
            throw socketError(
                "fcntl(F_SETFL, O_NONBLOCK)");
        }
#endif
    } catch (...) {
#ifdef _WIN32
        closesocket(
            static_cast<SOCKET>(handle_));
        handle_ = kInvalidHandle;

        WSACleanup();
        winsockInitialized_ = false;
#else
        ::close(
            static_cast<int>(handle_));
        handle_ = kInvalidHandle;
#endif
        throw;
    }
}

UdpSender::~UdpSender() {
    if (handle_ != kInvalidHandle) {
#ifdef _WIN32
        closesocket(
            static_cast<SOCKET>(handle_));
#else
        ::close(
            static_cast<int>(handle_));
#endif
        handle_ = kInvalidHandle;
    }

#ifdef _WIN32
    if (winsockInitialized_) {
        WSACleanup();
        winsockInitialized_ = false;
    }
#endif
}

bool UdpSender::trySend(
    std::span<const std::uint8_t> datagram) {
    if (datagram.empty()) {
        throw std::invalid_argument(
            "UDP datagram must not be empty");
    }

    if (datagram.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        throw std::length_error(
            "UDP datagram is too large");
    }

#ifdef _WIN32
    const int result =
        ::send(
            static_cast<SOCKET>(handle_),
            reinterpret_cast<
                const char*>(datagram.data()),
            static_cast<int>(
                datagram.size()),
            0);

    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();

        if (error == WSAEWOULDBLOCK) {
            return false;
        }

        throw socketError("send(UDP)");
    }

    if (result !=
        static_cast<int>(datagram.size())) {
        throw std::runtime_error(
            "UDP send returned a partial datagram");
    }
#else
    const ssize_t result =
        ::send(
            static_cast<int>(handle_),
            datagram.data(),
            datagram.size(),
            0);

    if (result < 0) {
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            return false;
        }

        throw socketError("send(UDP)");
    }

    if (static_cast<std::size_t>(result) !=
        datagram.size()) {
        throw std::runtime_error(
            "UDP send returned a partial datagram");
    }
#endif

    return true;
}

} // namespace reg::platform
