#include "metadata/UdpMetadataReceiver.hpp"

#include "metadata/MetadataProtocol.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace reg::metadata {
namespace {

std::runtime_error socketError(const char* operation) {
#ifdef _WIN32
    return std::runtime_error(
        std::string(operation) +
        " failed with WSA error " +
        std::to_string(WSAGetLastError()));
#else
    return std::runtime_error(
        std::string(operation) +
        " failed: " +
        std::strerror(errno));
#endif
}

} // namespace

UdpMetadataReceiver::UdpMetadataReceiver(
    UdpMetadataReceiverConfig config)
    : config_(std::move(config)) {
    if (config_.receiveTimeout.count() <= 0) {
        throw std::invalid_argument(
            "UDP metadata receive timeout must be greater than zero");
    }

    try {
        initializeSocket();
    } catch (...) {
        closeSocket();
#ifdef _WIN32
        if (winsockInitialized_) {
            WSACleanup();
            winsockInitialized_ = false;
        }
#endif
        throw;
    }
}

UdpMetadataReceiver::~UdpMetadataReceiver() {
    requestStop();
    closeSocket();

#ifdef _WIN32
    if (winsockInitialized_) {
        WSACleanup();
        winsockInitialized_ = false;
    }
#endif
}

void UdpMetadataReceiver::initializeSocket() {
#ifdef _WIN32
    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        throw std::runtime_error(
            "WSAStartup failed with error " +
            std::to_string(startupResult));
    }
    winsockInitialized_ = true;

    const SOCKET nativeSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (nativeSocket == INVALID_SOCKET) {
        throw socketError("socket(AF_INET, SOCK_DGRAM)");
    }
    socket_ = static_cast<NativeSocket>(nativeSocket);
#else
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        throw socketError("socket(AF_INET, SOCK_DGRAM)");
    }
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);

    if (config_.bindAddress.empty() ||
        config_.bindAddress == "0.0.0.0" ||
        config_.bindAddress == "*") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        const int parseResult =
            inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr);
        if (parseResult != 1) {
            closeSocket();
            throw std::invalid_argument(
                "Invalid IPv4 metadata bind address: " +
                config_.bindAddress);
        }
    }

#ifdef _WIN32
    if (::bind(
            static_cast<SOCKET>(socket_),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
        const auto error = socketError("bind(metadata UDP)");
        closeSocket();
        throw error;
    }

    const DWORD timeoutMs =
        static_cast<DWORD>(config_.receiveTimeout.count());
    if (setsockopt(
            static_cast<SOCKET>(socket_),
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMs),
            static_cast<int>(sizeof(timeoutMs))) == SOCKET_ERROR) {
        const auto error = socketError("setsockopt(SO_RCVTIMEO)");
        closeSocket();
        throw error;
    }
#else
    if (::bind(
            socket_,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) < 0) {
        const auto error = socketError("bind(metadata UDP)");
        closeSocket();
        throw error;
    }

    const auto timeoutUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            config_.receiveTimeout);
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(
        timeoutUs.count() / 1'000'000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
        timeoutUs.count() % 1'000'000);

    if (setsockopt(
            socket_,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) < 0) {
        const auto error = socketError("setsockopt(SO_RCVTIMEO)");
        closeSocket();
        throw error;
    }
#endif

    sockaddr_in bound{};
#ifdef _WIN32
    int boundSize = static_cast<int>(sizeof(bound));
    if (getsockname(
            static_cast<SOCKET>(socket_),
            reinterpret_cast<sockaddr*>(&bound),
            &boundSize) == SOCKET_ERROR) {
        const auto error = socketError("getsockname(metadata UDP)");
        closeSocket();
        throw error;
    }
#else
    socklen_t boundSize = sizeof(bound);
    if (getsockname(
            socket_,
            reinterpret_cast<sockaddr*>(&bound),
            &boundSize) < 0) {
        const auto error = socketError("getsockname(metadata UDP)");
        closeSocket();
        throw error;
    }
#endif

    boundPort_ = ntohs(bound.sin_port);
}

void UdpMetadataReceiver::run(const Callback& onMetadata) {
    if (!onMetadata) {
        throw std::invalid_argument(
            "UdpMetadataReceiver::run requires a callback");
    }
    if (socket_ == kInvalidSocket) {
        throw std::logic_error(
            "UdpMetadataReceiver socket is not initialized");
    }

    stopRequested_.store(false, std::memory_order_release);
    sequenceTracker_.reset();

    // Oversized datagrams are intentionally read into a buffer larger than the
    // protocol limit so they can be classified as invalid instead of appearing
    // as valid truncated packets.
    std::array<std::uint8_t, wire::kMaxDatagramSize + 512> buffer{};

    while (!stopRequested_.load(std::memory_order_acquire)) {
        sockaddr_in sender{};

#ifdef _WIN32
        int senderSize = static_cast<int>(sizeof(sender));
        const int received = recvfrom(
            static_cast<SOCKET>(socket_),
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderSize);

        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT ||
                error == WSAEWOULDBLOCK ||
                error == WSAEINTR) {
                continue;
            }
            if (error == WSAEMSGSIZE) {
                datagramsReceived_.fetch_add(1, std::memory_order_relaxed);
                invalidPackets_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            throw socketError("recvfrom(metadata UDP)");
        }

        const std::size_t receivedSize =
            static_cast<std::size_t>(received);
#else
        socklen_t senderSize = sizeof(sender);
        const ssize_t received = recvfrom(
            socket_,
            buffer.data(),
            buffer.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderSize);

        if (received < 0) {
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK ||
                errno == EINTR) {
                continue;
            }
            throw socketError("recvfrom(metadata UDP)");
        }

        const std::size_t receivedSize =
            static_cast<std::size_t>(received);
#endif

        datagramsReceived_.fetch_add(1, std::memory_order_relaxed);

        const auto result = wire::decodeFrameMetadata(
            std::span<const std::uint8_t>(
                buffer.data(),
                receivedSize),
            std::chrono::steady_clock::now());

        if (!result) {
            invalidPackets_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        handleAcceptedSequence(result.metadata->packetSequence);
        packetsAccepted_.fetch_add(1, std::memory_order_relaxed);
        onMetadata(result.metadata);
    }
}

void UdpMetadataReceiver::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

UdpMetadataReceiverStats UdpMetadataReceiver::stats() const noexcept {
    return UdpMetadataReceiverStats{
        .datagramsReceived =
            datagramsReceived_.load(std::memory_order_relaxed),
        .packetsAccepted =
            packetsAccepted_.load(std::memory_order_relaxed),
        .invalidPackets =
            invalidPackets_.load(std::memory_order_relaxed),
        .sequenceDuplicates =
            sequenceDuplicates_.load(std::memory_order_relaxed),
        .sequenceOutOfOrder =
            sequenceOutOfOrder_.load(std::memory_order_relaxed),
        .sequenceStale =
            sequenceStale_.load(std::memory_order_relaxed),
        .sequenceGaps =
            sequenceGaps_.load(std::memory_order_relaxed),
        .estimatedMissingPackets =
            estimatedMissingPackets_.load(std::memory_order_relaxed),
    };
}

void UdpMetadataReceiver::handleAcceptedSequence(
    std::uint32_t sequence) noexcept {
    const SequenceObservation observation =
        sequenceTracker_.observe(sequence);

    switch (observation.disposition) {
    case SequenceDisposition::First:
    case SequenceDisposition::InOrder:
        break;
    case SequenceDisposition::Gap:
        sequenceGaps_.fetch_add(1, std::memory_order_relaxed);
        estimatedMissingPackets_.fetch_add(
            observation.missingBeforeThisPacket,
            std::memory_order_relaxed);
        break;
    case SequenceDisposition::OutOfOrder:
        sequenceOutOfOrder_.fetch_add(1, std::memory_order_relaxed);
        break;
    case SequenceDisposition::Duplicate:
        sequenceDuplicates_.fetch_add(1, std::memory_order_relaxed);
        break;
    case SequenceDisposition::Stale:
        sequenceStale_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void UdpMetadataReceiver::closeSocket() noexcept {
    if (socket_ == kInvalidSocket) {
        return;
    }

#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket_));
#else
    ::close(socket_);
#endif
    socket_ = kInvalidSocket;
}

} // namespace reg::metadata
