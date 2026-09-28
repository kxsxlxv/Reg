#pragma once

#include <cstdint>
#include <optional>

namespace reg::metadata {

struct SequenceObservation {
    bool duplicate{};
    bool outOfOrder{};
    std::uint32_t missingBefore{};
};

class PacketSequenceTracker final {
public:
    SequenceObservation observe(std::uint32_t sequence) noexcept {
        if (!latest_) {
            latest_ = sequence;
            return {};
        }

        const std::uint32_t delta = sequence - *latest_;
        if (delta == 0) {
            return {.duplicate = true};
        }

        // Unsigned subtraction naturally handles wrap-around. Values in the
        // forward half of uint32 space are newer; values in the backward half
        // are late/out-of-order.
        if (delta < 0x80000000U) {
            latest_ = sequence;
            return {
                .duplicate = false,
                .outOfOrder = false,
                .missingBefore = delta > 1 ? delta - 1 : 0,
            };
        }

        return {.outOfOrder = true};
    }

    void reset() noexcept {
        latest_.reset();
    }

    std::optional<std::uint32_t> latest() const noexcept {
        return latest_;
    }

private:
    std::optional<std::uint32_t> latest_;
};

} // namespace reg::metadata
