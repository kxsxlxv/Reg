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

    // Moves an existing window to the center of the requested display while
    // preserving its current windowed size. Returns false if that ordinal is
    // not currently connected.
    bool placeWindowOnDisplay(
        SDL_Window* window,
        std::size_t displayOrdinal);

    bool pollQuitRequested();

    bool takeDisplayTopologyChanged() noexcept {
        const bool changed =
            displayTopologyChanged_;
        displayTopologyChanged_ = false;
        return changed;
    }

private:
    std::vector<SDL_Window*> windows_;
    bool displayTopologyChanged_{false};
};

} // namespace reg::platform
