#include "platform/UdpSocket.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace reg::platform {
namespace {

constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};

#ifdef _WIN32
SOCKET nativeSocket(std::uintptr_t value) noexcept {
    return static_cast<SOCKET>(value);
}

std::runtime_error socketError(const char* operation, int error) {
    return std::runtime_error(
        std::string(operation) + " failed with Winsock error " +
        std::to_string(error));
}
#else
int nativeSocket(std::uintptr_t value) noexcept {
    return static_cast<int>(value);
}

std::runtime_error socketError(const char* operation, int error) {
    return std::runtime_error(
        std::string(operation) + " failed with errno " +
        std::to_string(error));
}
#endif

} // namespace

UdpSocket::UdpSocket(
    const std::string& bindAddress,
    std::uint16_t port,
    std::chrono::milliseconds receiveTimeout,
    int receiveBufferBytes) {
    if (receiveTimeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("UDP receive timeout must be positive");
    }
    if (receiveBufferBytes <= 0) {
        throw std::invalid_argument("UDP receive buffer must be positive");
    }

#ifdef _WIN32
    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        throw socketError("WSAStartup", startupResult);
    }
    winsockStarted_ = true;

    const SOCKET created = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (created == INVALID_SOCKET) {
        const int error = WSAGetLastError();
        close();
        throw socketError("socket(AF_INET, SOCK_DGRAM)", error);
    }
    socket_ = static_cast<std::uintptr_t>(created);
#else
    const int created = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (created < 0) {
        throw socketError("socket(AF_INET, SOCK_DGRAM)", errno);
    }
    socket_ = static_cast<std::uintptr_t>(created);
#endif

    try {
        const int clampedBuffer =
            std::min(receiveBufferBytes, std::numeric_limits<int>::max());

#ifdef _WIN32
        const int receiveBufferOptionSize = static_cast<int>(sizeof(clampedBuffer));
#else
        const socklen_t receiveBufferOptionSize =
            static_cast<socklen_t>(sizeof(clampedBuffer));
#endif

        if (::setsockopt(
                nativeSocket(socket_),
                SOL_SOCKET,
                SO_RCVBUF,
#ifdef _WIN32
                reinterpret_cast<const char*>(&clampedBuffer),
#else
                &clampedBuffer,
#endif
                receiveBufferOptionSize) != 0) {
#ifdef _WIN32
            throw socketError("setsockopt(SO_RCVBUF)", WSAGetLastError());
#else
            throw socketError("setsockopt(SO_RCVBUF)", errno);
#endif
        }

#ifdef _WIN32
        const DWORD timeoutMs = static_cast<DWORD>(
            std::min<std::int64_t>(
                receiveTimeout.count(),
                static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())));
        if (::setsockopt(
                nativeSocket(socket_),
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                static_cast<int>(sizeof(timeoutMs))) != 0) {
            throw socketError("setsockopt(SO_RCVTIMEO)", WSAGetLastError());
        }
#else
        const auto totalMicroseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(receiveTimeout);
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(totalMicroseconds.count() / 1'000'000);
        timeout.tv_usec = static_cast<suseconds_t>(totalMicroseconds.count() % 1'000'000);
        if (::setsockopt(
                nativeSocket(socket_),
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                static_cast<socklen_t>(sizeof(timeout))) != 0) {
            throw socketError("setsockopt(SO_RCVTIMEO)", errno);
        }
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        if (bindAddress.empty() || bindAddress == "0.0.0.0") {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            const int parseResult =
                ::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr);
            if (parseResult != 1) {
                throw std::invalid_argument(
                    "Metadata UDP bind address must be a valid IPv4 address");
            }
        }

        if (::bind(
                nativeSocket(socket_),
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address))) != 0) {
#ifdef _WIN32
            throw socketError("bind(metadata UDP)", WSAGetLastError());
#else
            throw socketError("bind(metadata UDP)", errno);
#endif
        }

        sockaddr_in local{};
#ifdef _WIN32
        int localSize = static_cast<int>(sizeof(local));
#else
        socklen_t localSize = static_cast<socklen_t>(sizeof(local));
#endif
        if (::getsockname(
                nativeSocket(socket_),
                reinterpret_cast<sockaddr*>(&local),
                &localSize) != 0) {
#ifdef _WIN32
            throw socketError("getsockname(metadata UDP)", WSAGetLastError());
#else
            throw socketError("getsockname(metadata UDP)", errno);
#endif
        }

        localPort_ = ntohs(local.sin_port);
    } catch (...) {
        close();
        throw;
    }
}

UdpSocket::~UdpSocket() {
    close();
}

std::optional<std::size_t> UdpSocket::receive(
    std::span<std::uint8_t> buffer) {
    if (buffer.empty()) {
        throw std::invalid_argument("UDP receive buffer is empty");
    }

#ifdef _WIN32
    const int length = static_cast<int>(
        std::min<std::size_t>(
            buffer.size(),
            static_cast<std::size_t>(std::numeric_limits<int>::max())));

    const int result = ::recvfrom(
        nativeSocket(socket_),
        reinterpret_cast<char*>(buffer.data()),
        length,
        0,
        nullptr,
        nullptr);

    if (result >= 0) {
        return static_cast<std::size_t>(result);
    }

    const int error = WSAGetLastError();
    if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
        return std::nullopt;
    }
    if (error == WSAEMSGSIZE) {
        // Return the supplied buffer length. The metadata decoder will reject
        // it as oversized without trusting truncated contents.
        return buffer.size();
    }
    if (error == WSAEINTR) {
        return std::nullopt;
    }

    throw socketError("recvfrom(metadata UDP)", error);
#else
    const ssize_t result = ::recvfrom(
        nativeSocket(socket_),
        buffer.data(),
        buffer.size(),
        0,
        nullptr,
        nullptr);

    if (result >= 0) {
        return static_cast<std::size_t>(result);
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return std::nullopt;
    }

    throw socketError("recvfrom(metadata UDP)", errno);
#endif
}

void UdpSocket::close() noexcept {
    if (socket_ != kInvalidSocket) {
#ifdef _WIN32
        ::closesocket(nativeSocket(socket_));
#else
        ::close(nativeSocket(socket_));
#endif
        socket_ = kInvalidSocket;
    }

#ifdef _WIN32
    if (winsockStarted_) {
        WSACleanup();
        winsockStarted_ = false;
    }
#endif
}

} // namespace reg::platform
