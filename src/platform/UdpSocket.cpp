#include "platform/UdpSocket.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace reg::platform {
namespace {

#ifdef _WIN32
using SocketLength = int;

SOCKET nativeSocket(std::intptr_t value) noexcept {
    return static_cast<SOCKET>(value);
}

std::string socketError(const char* operation) {
    return std::string(operation) + " failed with WSA error " +
           std::to_string(WSAGetLastError());
}
#else
using SocketLength = socklen_t;

int nativeSocket(std::intptr_t value) noexcept {
    return static_cast<int>(value);
}

std::string socketError(const char* operation) {
    return std::string(operation) + " failed: " + std::strerror(errno);
}
#endif

} // namespace

UdpSocket::UdpSocket() {
#ifdef _WIN32
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error(
            "WSAStartup failed with error " + std::to_string(result));
    }
    winsockInitialized_ = true;
#endif
}

UdpSocket::~UdpSocket() {
    if (socket_ != -1) {
#ifdef _WIN32
        closesocket(nativeSocket(socket_));
#else
        close(nativeSocket(socket_));
#endif
        socket_ = -1;
    }

#ifdef _WIN32
    if (winsockInitialized_) {
        WSACleanup();
        winsockInitialized_ = false;
    }
#endif
}

void UdpSocket::bindIpv4(
    std::string_view address,
    std::uint16_t port) {
    if (socket_ != -1) {
        throw std::logic_error("UdpSocket is already bound");
    }

#ifdef _WIN32
    const SOCKET created = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (created == INVALID_SOCKET) {
        throw std::runtime_error(socketError("socket"));
    }
    socket_ = static_cast<std::intptr_t>(created);
#else
    const int created = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (created < 0) {
        throw std::runtime_error(socketError("socket"));
    }
    socket_ = static_cast<std::intptr_t>(created);
#endif

    int reuse = 1;
    if (setsockopt(
            nativeSocket(socket_),
            SOL_SOCKET,
            SO_REUSEADDR,
#ifdef _WIN32
            reinterpret_cast<const char*>(&reuse),
#else
            &reuse,
#endif
            static_cast<SocketLength>(sizeof(reuse))) != 0) {
        throw std::runtime_error(socketError("setsockopt(SO_REUSEADDR)"));
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);

    const std::string addressString(address);
    if (inet_pton(AF_INET, addressString.c_str(), &endpoint.sin_addr) != 1) {
        throw std::invalid_argument(
            "Invalid IPv4 metadata bind address: " + addressString);
    }

    if (::bind(
            nativeSocket(socket_),
            reinterpret_cast<const sockaddr*>(&endpoint),
            static_cast<SocketLength>(sizeof(endpoint))) != 0) {
        throw std::runtime_error(socketError("bind"));
    }
}

std::optional<std::size_t> UdpSocket::receive(
    std::span<std::uint8_t> buffer,
    std::chrono::milliseconds timeout) {
    if (socket_ == -1) {
        throw std::logic_error("UdpSocket::receive called before bind");
    }
    if (buffer.empty()) {
        throw std::invalid_argument("UdpSocket::receive requires a non-empty buffer");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("UDP receive timeout must be non-negative");
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(nativeSocket(socket_), &readSet);

    const auto timeoutUs =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutUs.count() / 1'000'000);
    tv.tv_usec = static_cast<long>(timeoutUs.count() % 1'000'000);

#ifdef _WIN32
    const int selectResult =
        select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int selectResult =
        select(nativeSocket(socket_) + 1, &readSet, nullptr, nullptr, &tv);
#endif

    if (selectResult == 0) {
        return std::nullopt;
    }
    if (selectResult < 0) {
#ifndef _WIN32
        if (errno == EINTR) {
            return std::nullopt;
        }
#endif
        throw std::runtime_error(socketError("select"));
    }

#ifdef _WIN32
    const int received = recvfrom(
        nativeSocket(socket_),
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        nullptr,
        nullptr);
    if (received == SOCKET_ERROR) {
        throw std::runtime_error(socketError("recvfrom"));
    }
    return static_cast<std::size_t>(received);
#else
    const ssize_t received = recvfrom(
        nativeSocket(socket_),
        buffer.data(),
        buffer.size(),
        0,
        nullptr,
        nullptr);
    if (received < 0) {
        if (errno == EINTR) {
            return std::nullopt;
        }
        throw std::runtime_error(socketError("recvfrom"));
    }
    return static_cast<std::size_t>(received);
#endif
}

} // namespace reg::platform
