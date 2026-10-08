#pragma once

#include "remote/RemoteInputState.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace reg::platform {

struct DisplayInfo {
    std::uint32_t id{};
    std::string name;
    int x{};
    int y{};
    int width{};
    int height{};
};

class SDLPlatform {
public:
    SDLPlatform();
    ~SDLPlatform();

    SDLPlatform(const SDLPlatform&) = delete;
    SDLPlatform& operator=(const SDLPlatform&) = delete;

    SDL_Window* createVulkanWindow(
        const char* title,
        int width,
        int height,
        bool focusable = true);

    std::vector<DisplayInfo> displays() const;

    // Optional positive override selects the Windows Settings display number
    // (or 1-based SDL ordinal on Linux); zero uses the original mapping.
    // Call sites pass logical roles in creation order: Raw=0, Overlay=1,
    // Telemetry=2. On Windows these are mapped to the numbers shown by
    // Settings -> System -> Display -> Identify using active CCD
    // QueryDisplayConfig path ordering: Raw=3, Overlay=2, Telemetry=1.
    // Placement uses normal borderless monitor-sized windows rather than OS
    // fullscreen state. Secondary output windows are created non-focusable so
    // clicking them cannot trigger a foreground/focus transition between the
    // three Vulkan presentation surfaces.
    bool placeWindowOnDisplay(
        SDL_Window* window,
        std::size_t logicalRole,
        int displayNumberOverride = 0);

    // Portable ordinal fallback used outside Windows. Despite the retained API
    // name this uses a borderless monitor-sized window, not SDL fullscreen.
    bool placeWindowFullscreenOnDisplay(
        SDL_Window* window,
        std::size_t displayOrdinal);

    bool pollQuitRequested();

    bool takeRawScreenshotRequested() noexcept {
        const bool requested =
            rawScreenshotRequested_;
        rawScreenshotRequested_ = false;
        return requested;
    }

    bool takeOverlayScreenshotRequested() noexcept {
        const bool requested =
            overlayScreenshotRequested_;
        overlayScreenshotRequested_ = false;
        return requested;
    }

    bool takeDisplayTopologyChanged() noexcept {
        const bool changed =
            displayTopologyChanged_;
        displayTopologyChanged_ = false;
        return changed;
    }

private:
    std::vector<SDL_Window*> windows_;
    std::uint32_t rawWindowId_{};
    remote::RemoteUiInputState remoteUiInput_{};
    bool displayTopologyChanged_{false};
    bool rawScreenshotRequested_{false};
    bool overlayScreenshotRequested_{false};
};

} // namespace reg::platform
