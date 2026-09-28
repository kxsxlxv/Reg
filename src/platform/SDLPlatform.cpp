#include "platform/SDLPlatform.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace reg::platform {

SDLPlatform::SDLPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
}

SDLPlatform::~SDLPlatform() {
    for (SDL_Window* window : windows_) {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
    windows_.clear();
    SDL_Quit();
}

SDL_Window* SDLPlatform::createVulkanWindow(
    const char* title,
    int width,
    int height) {
    SDL_Window* window = SDL_CreateWindow(
        title,
        width,
        height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    windows_.push_back(window);
    return window;
}

PlatformEvents SDLPlatform::pollEvents() {
    PlatformEvents events{};

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            events.quitRequested = true;
        }

        if (event.type == SDL_EVENT_DISPLAY_ADDED ||
            event.type == SDL_EVENT_DISPLAY_REMOVED ||
            event.type == SDL_EVENT_DISPLAY_MOVED ||
            event.type == SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED ||
            event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ||
            event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED ||
            event.type == SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED) {
            events.displayTopologyChanged = true;
        }

        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
            event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
            events.windowDisplayChanged = true;
        }
    }

    return events;
}

} // namespace reg::platform
