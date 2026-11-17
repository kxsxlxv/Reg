#pragma once
#include "cvv1/Cvv1.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace reg::cvv1 {

struct ReceiverConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{50011};
    std::uint32_t timeoutMs{100};
};
struct ReceiverStats {
    std::uint64_t datagrams{}, invalid{}, completed{};
};

class Receiver final {
public:
    Receiver(Store& store, ReceiverConfig config = {})
        : store_(store), config_(std::move(config)) {}
    void run();
    void stop() noexcept { stopRequested_.store(true, std::memory_order_release); }
    ReceiverStats stats() const noexcept {
        return {datagrams_.load(), invalid_.load(), completed_.load()};
    }
    std::int64_t lastValidReceiveNs() const noexcept {
        return lastValidReceiveNs_.load(std::memory_order_acquire);
    }
private:
    Store& store_;
    ReceiverConfig config_;
    std::atomic_bool stopRequested_{false};
    std::atomic_uint64_t datagrams_{0}, invalid_{0}, completed_{0};
    std::atomic<std::int64_t> lastValidReceiveNs_{0};
};
} // namespace reg::cvv1
