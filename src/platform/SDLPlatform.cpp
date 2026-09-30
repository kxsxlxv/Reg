#include "platform/SDLPlatform.hpp"

#include "video/SignalState.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace reg::platform {
namespace {

std::size_t physicalDisplayOrdinal(
    const std::size_t roleOrdinal,
    const std::size_t displayCount) noexcept {
    if (displayCount < 3U) {
        return roleOrdinal;
    }

    switch (roleOrdinal) {
    case 0U: return 2U; // Raw -> monitor 3.
    case 1U: return 1U; // Overlay -> monitor 2.
    case 2U: return 0U; // Telemetry -> monitor 1 (portrait).
    default: return roleOrdinal;
    }
}

bool isVideoRoleTitle(const char* title) noexcept {
    if (title == nullptr) {
        return false;
    }
    const std::string value(title);
    return value.find("Raw") != std::string::npos ||
           value.find("Overlay") != std::string::npos;
}

} // namespace

SDLPlatform::SDLPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(
            std::string("SDL_Init failed: ") +
            SDL_GetError());
    }
}

SDLPlatform::~SDLPlatform() {
    for (auto& cover : signalCovers_) {
        if (cover.renderer != nullptr) {
            SDL_DestroyRenderer(cover.renderer);
            cover.renderer = nullptr;
        }
        if (cover.window != nullptr) {
            SDL_DestroyWindow(cover.window);
            cover.window = nullptr;
        }
    }
    signalCovers_.clear();

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

    if (isVideoRoleTitle(title)) {
        try {
            createSignalCover(window, title);
        } catch (...) {
            windows_.pop_back();
            SDL_DestroyWindow(window);
            throw;
        }
    }

    return window;
}

void SDLPlatform::createSignalCover(
    SDL_Window* videoWindow,
    const char* title) {
    const std::string coverTitle =
        std::string(title != nullptr ? title : "Reg") +
        " - NO SIGNAL";

    SDL_Window* coverWindow = SDL_CreateWindow(
        coverTitle.c_str(),
        640,
        360,
        SDL_WINDOW_HIDDEN |
            SDL_WINDOW_BORDERLESS |
            SDL_WINDOW_ALWAYS_ON_TOP |
            SDL_WINDOW_UTILITY |
            SDL_WINDOW_NOT_FOCUSABLE);

    if (coverWindow == nullptr) {
        throw std::runtime_error(
            std::string("SDL_CreateWindow(no-signal) failed: ") +
            SDL_GetError());
    }

    SDL_Renderer* renderer =
        SDL_CreateRenderer(coverWindow, nullptr);

    if (renderer == nullptr) {
        SDL_DestroyWindow(coverWindow);
        throw std::runtime_error(
            std::string("SDL_CreateRenderer(no-signal) failed: ") +
            SDL_GetError());
    }

    if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
        !SDL_RenderClear(renderer) ||
        !SDL_RenderPresent(renderer)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(coverWindow);
        throw std::runtime_error(
            std::string("failed to initialize no-signal renderer: ") +
            SDL_GetError());
    }

    signalCovers_.push_back(SignalCover{
        .videoWindow = videoWindow,
        .window = coverWindow,
        .renderer = renderer,
        .visible = false,
    });
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

void SDLPlatform::placeSignalCover(
    SDL_Window* videoWindow,
    const DisplayInfo& display) {
    const auto found = std::ranges::find_if(
        signalCovers_,
        [videoWindow](const SignalCover& cover) {
            return cover.videoWindow == videoWindow;
        });

    if (found == signalCovers_.end()) {
        return;
    }

    if (!SDL_SetWindowPosition(
            found->window,
            display.x,
            display.y) ||
        !SDL_SetWindowSize(
            found->window,
            display.width,
            display.height)) {
        throw std::runtime_error(
            std::string("failed to place no-signal cover: ") +
            SDL_GetError());
    }
}

bool SDLPlatform::placeWindowOnDisplay(
    SDL_Window* window,
    std::size_t displayOrdinal) {
    if (window == nullptr) {
        throw std::invalid_argument(
            "placeWindowOnDisplay requires a window");
    }

    const auto connected =
        displays();

    const std::size_t physicalOrdinal =
        physicalDisplayOrdinal(
            displayOrdinal,
            connected.size());

    if (physicalOrdinal >= connected.size()) {
        return false;
    }

    const auto& display =
        connected[physicalOrdinal];

    // SDL chooses the fullscreen display from the window position. Leave
    // desktop mode selected so the native monitor resolution/orientation is
    // used instead of a synthetic video mode.
    if (!SDL_SetWindowFullscreen(window, false)) {
        throw std::runtime_error(
            std::string("SDL_SetWindowFullscreen(false) failed: ") +
            SDL_GetError());
    }

    if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
        throw std::runtime_error(
            std::string("SDL_SetWindowFullscreenMode(desktop) failed: ") +
            SDL_GetError());
    }

    if (!SDL_SetWindowPosition(
            window,
            display.x,
            display.y)) {
        throw std::runtime_error(
            std::string("SDL_SetWindowPosition failed: ") +
            SDL_GetError());
    }

    if (!SDL_SetWindowSize(
            window,
            display.width,
            display.height)) {
        throw std::runtime_error(
            std::string("SDL_SetWindowSize failed: ") +
            SDL_GetError());
    }

    if (!SDL_SetWindowFullscreen(window, true)) {
        throw std::runtime_error(
            std::string("SDL_SetWindowFullscreen(true) failed: ") +
            SDL_GetError());
    }

    placeSignalCover(window, display);
    updateSignalCovers();
    return true;
}

void SDLPlatform::updateSignalCovers() {
    const bool signalPresent =
        video::SignalState::recent();

    for (auto& cover : signalCovers_) {
        if (signalPresent) {
            if (cover.visible) {
                static_cast<void>(
                    SDL_HideWindow(cover.window));
                cover.visible = false;
            }
            continue;
        }

        if (!cover.visible) {
            static_cast<void>(
                SDL_SetRenderDrawColor(
                    cover.renderer,
                    0,
                    0,
                    0,
                    255));
            static_cast<void>(
                SDL_RenderClear(cover.renderer));
            static_cast<void>(
                SDL_RenderPresent(cover.renderer));
            static_cast<void>(
                SDL_ShowWindow(cover.window));
            static_cast<void>(
                SDL_RaiseWindow(cover.window));
            cover.visible = true;
        }
    }
}

bool SDLPlatform::pollQuitRequested() {
    updateSignalCovers();

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
