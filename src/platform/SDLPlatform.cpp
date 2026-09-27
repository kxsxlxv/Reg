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
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
    SDL_Quit();
}

SDL_Window* SDLPlatform::createVulkanWindow(const char* title, int width, int height) {
    if (window_ != nullptr) {
        throw std::runtime_error("The Phase A probe supports one bootstrap window");
    }

    window_ = SDL_CreateWindow(
        title,
        width,
        height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (window_ == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    return window_;
}

bool SDLPlatform::pollQuitRequested() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            return true;
        }
    }
    return false;
}

} // namespace reg::platform
