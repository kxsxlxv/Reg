#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

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

    // Places a role window in borderless desktop fullscreen. The legacy role
    // ordinals used by main are intentionally mapped to the operator layout:
    //   0 (raw)       -> physical monitor 3
    //   1 (overlay)   -> physical monitor 2
    //   2 (telemetry) -> physical monitor 1
    // With fewer than three displays, ordinals fall back to SDL order.
    bool placeWindowOnDisplay(
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
    struct SignalCover {
        SDL_Window* videoWindow{};
        SDL_Window* window{};
        SDL_Renderer* renderer{};
        bool visible{};
    };

    void createSignalCover(SDL_Window* videoWindow, const char* title);
    void placeSignalCover(SDL_Window* videoWindow, const DisplayInfo& display);
    void updateSignalCovers();

    std::vector<SDL_Window*> windows_;
    std::vector<SignalCover> signalCovers_;
    bool displayTopologyChanged_{false};
    bool rawScreenshotRequested_{false};
    bool overlayScreenshotRequested_{false};
};

} // namespace reg::platform
