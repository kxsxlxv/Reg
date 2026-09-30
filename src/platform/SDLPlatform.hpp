#pragma once

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
        int height);

    std::vector<DisplayInfo> displays() const;

    // Existing Reg call sites pass logical output roles in creation order:
    // Raw=0, Overlay=1, Telemetry=2. This maps those roles to the requested
    // physical monitor layout (3, 2, 1 respectively) and uses borderless
    // fullscreen desktop. Returns false when the target monitor is unavailable.
    bool placeWindowOnDisplay(
        SDL_Window* window,
        std::size_t displayOrdinal);

    // Places the window on the requested physical display ordinal and switches
    // it to SDL3 borderless fullscreen-desktop mode. This deliberately does not
    // request an exclusive display mode, so each monitor keeps its native
    // desktop timing/resolution and Windows multi-monitor topology remains intact.
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
    bool displayTopologyChanged_{false};
    bool rawScreenshotRequested_{false};
    bool overlayScreenshotRequested_{false};
};

} // namespace reg::platform
