#include "platform/DisplayLayout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace reg::platform {
namespace {

class DisplayList final {
public:
    DisplayList() {
        displays_ = SDL_GetDisplays(&count_);
        if (displays_ == nullptr) {
            throw std::runtime_error(
                std::string("SDL_GetDisplays failed: ") +
                SDL_GetError());
        }
    }

    ~DisplayList() {
        SDL_free(displays_);
    }

    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;

    int count() const noexcept {
        return count_;
    }

    SDL_DisplayID at(int index) const {
        if (index < 0 || index >= count_) {
            return 0;
        }
        return displays_[index];
    }

private:
    SDL_DisplayID* displays_{nullptr};
    int count_{0};
};

void checkSdl(bool result, const char* operation) {
    if (!result) {
        throw std::runtime_error(
            std::string(operation) +
            " failed: " +
            SDL_GetError());
    }
}

} // namespace

DisplayPlacementResult DisplayLayout::apply(
    SDL_Window* window,
    DisplayPlacement placement) const {
    if (window == nullptr) {
        throw std::invalid_argument(
            "DisplayLayout requires a valid SDL window");
    }

    DisplayList displays;
    if (displays.count() <= 0) {
        throw std::runtime_error(
            "SDL reports no connected displays");
    }

    const bool fallback =
        placement.displayIndex >=
        static_cast<std::size_t>(displays.count());

    const int selectedIndex =
        fallback
            ? 0
            : static_cast<int>(placement.displayIndex);

    const SDL_DisplayID targetDisplay =
        displays.at(selectedIndex);

    SDL_Rect bounds{};
    checkSdl(
        SDL_GetDisplayBounds(
            targetDisplay,
            &bounds),
        "SDL_GetDisplayBounds");

    const SDL_DisplayID currentDisplay =
        SDL_GetDisplayForWindow(window);

    const bool isFullscreen =
        (SDL_GetWindowFlags(window) &
         SDL_WINDOW_FULLSCREEN) != 0;

    if (placement.fullscreen) {
        // Borderless desktop fullscreen can move between displays. Leave
        // exclusive mode unset so this remains compositor-friendly on Wayland.
        if (isFullscreen &&
            currentDisplay != 0 &&
            currentDisplay != targetDisplay) {
            checkSdl(
                SDL_SetWindowFullscreen(window, false),
                "SDL_SetWindowFullscreen(false)");
        }

        if (!isFullscreen ||
            currentDisplay != targetDisplay) {
            checkSdl(
                SDL_SetWindowPosition(
                    window,
                    bounds.x,
                    bounds.y),
                "SDL_SetWindowPosition(display)");

            checkSdl(
                SDL_SetWindowFullscreen(window, true),
                "SDL_SetWindowFullscreen(true)");

            checkSdl(
                SDL_SyncWindow(window),
                "SDL_SyncWindow(fullscreen)");
        }
    } else {
        if (isFullscreen) {
            checkSdl(
                SDL_SetWindowFullscreen(window, false),
                "SDL_SetWindowFullscreen(false)");

            checkSdl(
                SDL_SyncWindow(window),
                "SDL_SyncWindow(windowed)");
        }

        int width = 0;
        int height = 0;
        checkSdl(
            SDL_GetWindowSize(window, &width, &height),
            "SDL_GetWindowSize");

        const int x =
            bounds.x +
            std::max(
                0,
                (bounds.w - width) / 2);
        const int y =
            bounds.y +
            std::max(
                0,
                (bounds.h - height) / 2);

        if (currentDisplay != targetDisplay ||
            fallback) {
            checkSdl(
                SDL_SetWindowPosition(
                    window,
                    x,
                    y),
                "SDL_SetWindowPosition(center)");

            checkSdl(
                SDL_SyncWindow(window),
                "SDL_SyncWindow(position)");
        }
    }

    const SDL_DisplayID actualDisplay =
        SDL_GetDisplayForWindow(window);

    return DisplayPlacementResult{
        .requestedDisplayId =
            static_cast<std::uint32_t>(targetDisplay),
        .actualDisplayId =
            static_cast<std::uint32_t>(actualDisplay),
        .usedFallback = fallback,
    };
}

std::size_t DisplayLayout::displayCount() const {
    DisplayList displays;
    return static_cast<std::size_t>(
        std::max(displays.count(), 0));
}

} // namespace reg::platform
