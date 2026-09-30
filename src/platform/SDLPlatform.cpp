#include "platform/SDLPlatform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace reg::platform {

SDLPlatform::SDLPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(
            std::string("SDL_Init failed: ") +
            SDL_GetError());
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
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        throw std::runtime_error(
            std::string("SDL_CreateWindow failed: ") +
            SDL_GetError());
    }

    windows_.push_back(window);
    return window;
}

std::vector<DisplayInfo>
SDLPlatform::displays() const {
    int count = 0;
    SDL_DisplayID* ids =
        SDL_GetDisplays(&count);

    if (ids == nullptr) {
        throw std::runtime_error(
            std::string("SDL_GetDisplays failed: ") +
            SDL_GetError());
    }

    std::vector<DisplayInfo> result;
    result.reserve(
        static_cast<std::size_t>(
            std::max(count, 0)));

    for (int i = 0; i < count; ++i) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(
                ids[i],
                &bounds)) {
            SDL_free(ids);
            throw std::runtime_error(
                std::string("SDL_GetDisplayBounds failed: ") +
                SDL_GetError());
        }

        const char* name =
            SDL_GetDisplayName(ids[i]);

        result.push_back(
            DisplayInfo{
                .id =
                    static_cast<std::uint32_t>(
                        ids[i]),
                .name =
                    name != nullptr
                        ? std::string(name)
                        : std::string("Unknown"),
                .x = bounds.x,
                .y = bounds.y,
                .width = bounds.w,
                .height = bounds.h,
            });
    }

    SDL_free(ids);
    return result;
}

bool SDLPlatform::placeWindowOnDisplay(
    SDL_Window* window,
    std::size_t displayOrdinal) {
    // Existing call sites use logical roles in creation order:
    //   0 = Raw, 1 = Overlay, 2 = Telemetry.
    // Physical Windows monitor assignment requested for Reg is:
    //   Raw -> monitor 3, Overlay -> monitor 2, Telemetry -> monitor 1.
    constexpr std::size_t logicalToPhysical[]{2U, 1U, 0U};

    const std::size_t physicalOrdinal =
        displayOrdinal < std::size(logicalToPhysical)
            ? logicalToPhysical[displayOrdinal]
            : displayOrdinal;

    return placeWindowFullscreenOnDisplay(
        window,
        physicalOrdinal);
}

bool SDLPlatform::placeWindowFullscreenOnDisplay(
    SDL_Window* window,
    std::size_t displayOrdinal) {
    if (window == nullptr) {
        throw std::invalid_argument(
            "placeWindowFullscreenOnDisplay requires a window");
    }

    const auto connected = displays();
    if (displayOrdinal >= connected.size()) {
        return false;
    }

    const auto& display = connected[displayOrdinal];

    // SDL3 fullscreen defaults to borderless fullscreen desktop when no
    // exclusive fullscreen mode is selected. Move the window first so Windows
    // associates it with the intended monitor, then enter fullscreen.
    if (!SDL_SetWindowFullscreen(window, false)) {
        return false;
    }
    if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
        return false;
    }
    if (!SDL_SetWindowPosition(window, display.x, display.y)) {
        return false;
    }
    if (!SDL_SyncWindow(window)) {
        return false;
    }
    if (!SDL_SetWindowFullscreen(window, true)) {
        return false;
    }
    return SDL_SyncWindow(window);
}

bool SDLPlatform::pollQuitRequested() {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
        if (event.type ==
                SDL_EVENT_QUIT ||
            event.type ==
                SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            return true;
        }

        if (event.type ==
                SDL_EVENT_KEY_DOWN &&
            !event.key.repeat) {
            if (event.key.key == SDLK_F11) {
                rawScreenshotRequested_ = true;
            } else if (
                event.key.key == SDLK_F12) {
                overlayScreenshotRequested_ = true;
            }
        }

        if (event.type ==
                SDL_EVENT_DISPLAY_ADDED ||
            event.type ==
                SDL_EVENT_DISPLAY_REMOVED ||
            event.type ==
                SDL_EVENT_DISPLAY_MOVED ||
            event.type ==
                SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
            displayTopologyChanged_ = true;
        }
    }

    return false;
}

} // namespace reg::platform
