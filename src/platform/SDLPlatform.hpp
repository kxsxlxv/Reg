#pragma once

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
    SDL_Window* window_{nullptr};
};

} // namespace reg::platform
