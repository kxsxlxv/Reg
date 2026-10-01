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

    // Call sites pass logical roles in creation order: Raw=0, Overlay=1,
    // Telemetry=2. On Windows these are mapped to the numbers shown by
    // Settings -> System -> Display -> Identify using active CCD
    // QueryDisplayConfig path ordering: Raw=3, Overlay=2, Telemetry=1.
    // Placement uses normal borderless monitor-sized windows rather than OS
    // fullscreen state so focus changes do not trigger a fullscreen/swapchain
    // transition.
    bool placeWindowOnDisplay(
        SDL_Window* window,
        std::size_t logicalRole);

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
    bool displayTopologyChanged_{false};
    bool rawScreenshotRequested_{false};
    bool overlayScreenshotRequested_{false};
};

} // namespace reg::platform
