#pragma once

#include "media/GtSei.hpp"

#include <atomic>
#include <cstdint>

namespace reg::app {

enum class TelemetryPage : std::uint8_t {
    Overview,
    ExactSync,
    NetImgui,
    Gt,
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
            ? TelemetryPage::ExactSync
            : (current == TelemetryPage::ExactSync
                ? TelemetryPage::NetImgui
                : (current == TelemetryPage::NetImgui
                    ? TelemetryPage::Gt
                    : TelemetryPage::Overview)));
}

// Shared between the telemetry checkbox and the physical Raw-window T key.
inline std::atomic_bool gShowGtBoxes{false};
inline std::atomic_bool gShowGtIds{false};
inline std::atomic<media::GtStatus> gPresentedGtStatus{media::GtStatus::Missing};
inline std::atomic_uint64_t gGtReadyFrames{0};
inline std::atomic_uint64_t gGtNotReadyFrames{0};
inline std::atomic_uint64_t gGtMissingFrames{0};
inline std::atomic_uint64_t gGtParseErrors{0};
inline std::atomic_uint64_t gGtMismatchedFrames{0};

inline bool showGtBoxes() noexcept {
    return gShowGtBoxes.load(std::memory_order_relaxed);
}
inline bool showGtIds() noexcept {
    return gShowGtIds.load(std::memory_order_relaxed);
}
inline void setShowGtBoxes(bool value) noexcept {
    gShowGtBoxes.store(value, std::memory_order_relaxed);
}
inline void setShowGtIds(bool value) noexcept {
    gShowGtIds.store(value, std::memory_order_relaxed);
}
inline void toggleGtBoxes() noexcept {
    bool previous = gShowGtBoxes.load(std::memory_order_relaxed);
    while (!gShowGtBoxes.compare_exchange_weak(
        previous, !previous, std::memory_order_relaxed)) {}
}
inline void observeGt(media::GtStatus status) noexcept {
    switch (status) {
    case media::GtStatus::Ready:
        gGtReadyFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case media::GtStatus::NotReady:
    case media::GtStatus::Busy:
    case media::GtStatus::AmbiguousViewport:
    case media::GtStatus::Unavailable:
        gGtNotReadyFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case media::GtStatus::Missing:
        gGtMissingFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case media::GtStatus::ParseError:
        gGtParseErrors.fetch_add(1, std::memory_order_relaxed);
        break;
    case media::GtStatus::FrameMismatch:
        gGtMismatchedFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

} // namespace reg::app