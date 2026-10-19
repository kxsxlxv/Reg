#include "cvv1/Receiver.hpp"
#include "platform/UdpSocket.hpp"
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace reg::cvv1 {
void Receiver::run() {
    stopRequested_.store(false, std::memory_order_release);
    platform::UdpSocket socket(config_.bindAddress, config_.port, config_.timeoutMs);
    Reassembler fragments;
    // Oversize datagrams must not be interpreted as valid truncated packets.
    std::array<std::uint8_t, kMaxDatagram + 1> buffer{};
    while (!stopRequested_.load(std::memory_order_acquire)) {
        platform::UdpEndpoint peer{};
        const std::size_t received = socket.receiveFrom(buffer, peer);
        if (!received) continue;
        datagrams_.fetch_add(1, std::memory_order_relaxed);
        try {
            auto snapshot = fragments.accept(
                std::span<const std::uint8_t>(buffer.data(), received),
                peer, Clock::now());
            if (!snapshot) continue;
            lastValidReceiveNs_.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    snapshot->receivedAt.time_since_epoch()).count(),
                std::memory_order_release);
            store_.insert(std::move(*snapshot));
            completed_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception&) {
            invalid_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
} // namespace reg::cvv1
