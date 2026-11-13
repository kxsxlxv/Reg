#pragma once
// CVM1 v1 flags: semantic interpretation only; no wire-layout changes.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace reg::render::cvm1 {
inline constexpr std::uint16_t kDetector = 0x0001U;
inline constexpr std::uint16_t kPropagated = 0x0002U;
inline constexpr std::uint16_t kTentative = 0x0004U;
inline constexpr std::uint16_t kConfirmed = 0x0008U;
inline constexpr std::uint16_t kSotSelected = 0x0010U;
inline constexpr std::uint16_t kWeak = 0x0020U;
inline constexpr std::uint16_t kLost = 0x0040U;
inline constexpr std::uint16_t kPredicted = 0x0080U;
inline constexpr std::uint16_t kHeld = 0x0100U;
inline constexpr std::uint16_t kKnownMask = 0x01ffU;

// Proposed explicit non-persistent ID convention, pending Jetson ratification.
// Top-bit-set IDs are local to one source frame; never join them as trajectories.
inline constexpr std::uint64_t kEphemeralIdMask = UINT64_C(1) << 63;
inline bool isEphemeralId(std::uint64_t id) noexcept {
    return (id & kEphemeralIdMask) != 0U;
}
inline std::uint64_t frameLocalId(std::uint64_t index) noexcept {
    return kEphemeralIdMask | (index & ~kEphemeralIdMask);
}

enum class Kind : std::uint8_t {
    detection, tentative, confirmed, predicted, propagated,
    sot_active, sot_weak, fallback, Count
};
inline constexpr std::size_t kStyleCount = static_cast<std::size_t>(Kind::Count);
inline constexpr std::array<const char*, kStyleCount> kStyleKeys{
    "detection", "tentative", "confirmed", "predicted", "propagated",
    "sot_active", "sot_weak", "fallback"
};
enum class Pattern : std::uint8_t { solid, dashed, corners };
struct Rgba { float r{}, g{}, b{}, a{1.0F}; };
struct Style {
    Rgba color{};
    float width{2.0F};
    Pattern pattern{Pattern::solid};
    float markerSize{5.0F};
    std::string label;
    bool visible{true};
};
struct Rule { std::uint16_t mask{}, value{}; Kind kind{Kind::fallback}; };

inline Rgba fromRgb(unsigned rgb) noexcept {
    return {
        static_cast<float>((rgb >> 16) & 255U) / 255.0F,
        static_cast<float>((rgb >> 8) & 255U) / 255.0F,
        static_cast<float>(rgb & 255U) / 255.0F, 1.0F
    };
}
inline bool conflicting(std::uint16_t flags) noexcept {
    const unsigned life =
        static_cast<unsigned>((flags & kTentative) != 0U) +
        static_cast<unsigned>((flags & kConfirmed) != 0U) +
        static_cast<unsigned>((flags & kLost) != 0U);
    return life > 1U ||
        ((flags & (kPredicted | kHeld)) == (kPredicted | kHeld)) ||
        ((flags & (kSotSelected | kLost)) == (kSotSelected | kLost));
}
struct Palette {
    std::array<Style, kStyleCount> styles{};
    std::vector<Rule> rules{};
    Palette() {
        styles[0] = {fromRgb(0xF5C542), 2, Pattern::solid, 5, "detection", true};
        styles[1] = {fromRgb(0xFF963B), 2, Pattern::dashed, 5, "tentative", true};
        styles[2] = {fromRgb(0x25D36B), 3, Pattern::solid, 6, "confirmed", true};
        styles[3] = {fromRgb(0x38ADFF), 2, Pattern::solid, 5, "predicted", true};
        styles[4] = {fromRgb(0x38ADFF), 2, Pattern::solid, 5, "propagated", true};
        styles[5] = {fromRgb(0xEC51EE), 4, Pattern::corners, 7, "sot_active", true};
        styles[6] = {fromRgb(0xF04444), 2, Pattern::dashed, 6, "sot_weak", true};
        styles[7] = {fromRgb(0xAAB2BD), 2, Pattern::solid, 5, "fallback", true};
    }
    static constexpr std::size_t index(Kind k) noexcept {
        return static_cast<std::size_t>(k);
    }
    Kind resolve(std::uint16_t flags) const noexcept {
        flags &= kKnownMask;  // future unallocated bits are harmless.
        if (conflicting(flags)) return Kind::fallback;
        // Explicit configuration rules override precedence but not conflicts.
        for (const auto& rule : rules) {
            const auto mask = static_cast<std::uint16_t>(rule.mask & kKnownMask);
            if (mask != 0U && (flags & mask) == (rule.value & mask))
                return rule.kind;
        }
        if ((flags & kSotSelected) != 0U)
            return (flags & kWeak) != 0U ? Kind::sot_weak : Kind::sot_active;
        if ((flags & kTentative) != 0U) return Kind::tentative;
        if ((flags & kPredicted) != 0U) return Kind::predicted;
        if ((flags & kPropagated) != 0U) return Kind::propagated;
        if ((flags & kHeld) != 0U) return Kind::propagated;
        if ((flags & kConfirmed) != 0U) return Kind::confirmed;
        if ((flags & kDetector) != 0U) return Kind::detection;
        return Kind::fallback;
    }
    const Style& style(std::uint16_t flags) const noexcept {
        return styles[index(resolve(flags))];
    }
};
// Both render paths consume the same immutable, atomically replaced palette.
inline std::atomic<std::shared_ptr<const Palette>>& storage() {
    static std::atomic<std::shared_ptr<const Palette>> s{
        std::make_shared<const Palette>()};
    return s;
}
inline std::shared_ptr<const Palette> current() {
    return storage().load(std::memory_order_acquire);
}
inline void publish(std::shared_ptr<const Palette> palette) {
    if (palette) storage().store(std::move(palette), std::memory_order_release);
}
} // namespace reg::render::cvm1
