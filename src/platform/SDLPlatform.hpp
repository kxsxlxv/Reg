#pragma once

#include <vector>

struct SDL_Window;

namespace reg::platform {

struct PlatformEvents {
    bool quitRequested{false};
    bool displayTopologyChanged{false};
    bool windowDisplayChanged{false};
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

    PlatformEvents pollEvents();

private:
    std::vector<SDL_Window*> windows_;
};

} // namespace reg::platform
