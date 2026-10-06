#pragma once

#include <atomic>
#include <cstdint>

namespace reg::app {

enum class TelemetryPage : std::uint8_t {
    Overview,
    NetImgui,
};

inline std::atomic<TelemetryPage> gTelemetryPage{
    TelemetryPage::Overview};

inline TelemetryPage telemetryPage() noexcept {
    return gTelemetryPage.load(std::memory_order_relaxed);
}

inline void setTelemetryPage(TelemetryPage page) noexcept {
    gTelemetryPage.store(page, std::memory_order_relaxed);
}

inline void toggleTelemetryPage() noexcept {
    const TelemetryPage current = telemetryPage();
    setTelemetryPage(
        current == TelemetryPage::Overview
            ? TelemetryPage::NetImgui
            : TelemetryPage::Overview);
}

} // namespace reg::app