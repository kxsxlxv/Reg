#pragma once

#include <vector>

struct SDL_Window;

namespace reg::platform {

class SDLPlatform {
public:
    SDLPlatform();
    ~SDLPlatform();

    SDLPlatform(const SDLPlatform&) = delete;
    SDLPlatform& operator=(const SDLPlatform&) = delete;

    SDL_Window* createVulkanWindow(const char* title, int width, int height);
    bool pollQuitRequested();

private:
    std::vector<SDL_Window*> windows_;
};

} // namespace reg::platform
