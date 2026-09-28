#include "platform/UdpSocket.hpp"

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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace reg::platform {
namespace {

std::runtime_error socketError(const char* operation) {
#ifdef _WIN32
    return std::runtime_error(
        std::string(operation) + " failed with WSA error " +
        std::to_string(WSAGetLastError()));
#else
    return std::runtime_error(
        std::string(operation) + " failed with errno " +
        std::to_string(errno));
#endif
}

} // namespace

UdpSocket::UdpSocket(
    std::string_view bindAddress,
    std::uint16_t port,
    std::uint32_t receiveTimeoutMs) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw socketError("WSAStartup");
    }
    winsockInitialized_ = true;

    const SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        winsockInitialized_ = false;
        throw socketError("socket(AF_INET, SOCK_DGRAM)");
    }
    handle_ = static_cast<std::uintptr_t>(socketHandle);
#else
    const int socketHandle = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle < 0) {
        throw socketError("socket(AF_INET, SOCK_DGRAM)");
    }
    handle_ = static_cast<std::uintptr_t>(socketHandle);
#endif

    try {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        const std::string addressText(bindAddress);
        if (addressText.empty() || addressText == "0.0.0.0" || addressText == "*") {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            const int parsed =
                inet_pton(AF_INET, addressText.c_str(), &address.sin_addr);
            if (parsed != 1) {
                throw std::invalid_argument(
                    "UDP bind address must be an IPv4 address or 0.0.0.0");
            }
        }

#ifdef _WIN32
        const DWORD timeout = receiveTimeoutMs;
        if (setsockopt(
                static_cast<SOCKET>(handle_),
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout),
                sizeof(timeout)) == SOCKET_ERROR) {
            throw socketError("setsockopt(SO_RCVTIMEO)");
        }

        if (::bind(
                static_cast<SOCKET>(handle_),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
            throw socketError("bind(UDP)");
        }
#else
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(receiveTimeoutMs / 1000U);
        timeout.tv_usec =
            static_cast<suseconds_t>((receiveTimeoutMs % 1000U) * 1000U);
        if (setsockopt(
                static_cast<int>(handle_),
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)) != 0) {
            throw socketError("setsockopt(SO_RCVTIMEO)");
        }

        if (::bind(
                static_cast<int>(handle_),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
            throw socketError("bind(UDP)");
        }
#endif
    } catch (...) {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(handle_));
        handle_ = kInvalidHandle;
        WSACleanup();
        winsockInitialized_ = false;
#else
        ::close(static_cast<int>(handle_));
        handle_ = kInvalidHandle;
#endif
        throw;
    }
}

UdpSocket::~UdpSocket() {
    if (handle_ != kInvalidHandle) {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(handle_));
#else
        ::close(static_cast<int>(handle_));
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

std::size_t UdpSocket::receive(std::span<std::uint8_t> buffer) {
    if (buffer.empty()) {
        throw std::invalid_argument("UDP receive buffer must not be empty");
    }

#ifdef _WIN32
    const int result = recv(
        static_cast<SOCKET>(handle_),
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);

    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
            return 0;
        }
        throw socketError("recv(UDP)");
    }
    return result > 0 ? static_cast<std::size_t>(result) : 0;
#else
    const ssize_t result = recv(
        static_cast<int>(handle_),
        buffer.data(),
        buffer.size(),
        0);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        throw socketError("recv(UDP)");
    }
    return result > 0 ? static_cast<std::size_t>(result) : 0;
#endif
}

} // namespace reg::platform
