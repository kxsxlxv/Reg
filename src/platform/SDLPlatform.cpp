#include "platform/SDLPlatform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>
#include <vector>
#endif

namespace reg::platform {
namespace {

bool placeBorderlessWindow(
    SDL_Window* window,
    int x,
    int y,
    int width,
    int height) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & SDL_WINDOW_FULLSCREEN) != 0U &&
        !SDL_SetWindowFullscreen(window, false)) {
        return false;
    }

    // Deliberately use a normal borderless window rather than SDL fullscreen.
    // On Windows a real fullscreen state can cause WSI/swapchain transitions
    // when focus moves between our three outputs. A borderless window sized to
    // the monitor client rectangle is visually fullscreen without that focus
    // transition and therefore avoids the one-frame black flash.
    if (!SDL_SetWindowBordered(window, false)) {
        return false;
    }
    if (!SDL_SetWindowResizable(window, false)) {
        return false;
    }
    if (!SDL_SetWindowPosition(window, x, y)) {
        return false;
    }
    if (!SDL_SetWindowSize(window, width, height)) {
        return false;
    }
    return SDL_SyncWindow(window);
}

#ifdef _WIN32
struct WindowsMonitor final {
    int displayNumber{};
    RECT bounds{};
};

int parseWindowsDisplayNumber(const wchar_t* deviceName) noexcept {
    if (deviceName == nullptr) {
        return 0;
    }

    constexpr wchar_t prefix[] = L"\\\\.\\DISPLAY";
    constexpr std::size_t prefixLength =
        (sizeof(prefix) / sizeof(prefix[0])) - 1U;

    if (std::wcsncmp(deviceName, prefix, prefixLength) != 0) {
        return 0;
    }

    wchar_t* end = nullptr;
    const long value = std::wcstol(deviceName + prefixLength, &end, 10);
    if (end == deviceName + prefixLength || value <= 0L) {
        return 0;
    }
    return static_cast<int>(value);
}

BOOL CALLBACK collectWindowsMonitor(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM userData) {
    auto* monitors =
        reinterpret_cast<std::vector<WindowsMonitor>*>(userData);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    const int displayNumber =
        parseWindowsDisplayNumber(info.szDevice);
    if (displayNumber <= 0) {
        return TRUE;
    }

    monitors->push_back(
        WindowsMonitor{
            .displayNumber = displayNumber,
            .bounds = info.rcMonitor,
        });
    return TRUE;
}

std::optional<WindowsMonitor>
windowsMonitorByDisplayNumber(int displayNumber) {
    std::vector<WindowsMonitor> monitors;
    if (!EnumDisplayMonitors(
            nullptr,
            nullptr,
            &collectWindowsMonitor,
            reinterpret_cast<LPARAM>(&monitors))) {
        return std::nullopt;
    }

    const auto it = std::ranges::find_if(
        monitors,
        [displayNumber](const WindowsMonitor& monitor) {
            return monitor.displayNumber == displayNumber;
        });
    if (it == monitors.end()) {
        return std::nullopt;
    }
    return *it;
}
#endif

} // namespace

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
    std::size_t logicalRole) {
    if (window == nullptr) {
        throw std::invalid_argument(
            "placeWindowOnDisplay requires a window");
    }

    // Call sites use logical roles in creation order:
    //   0 = Raw, 1 = Overlay, 2 = Telemetry.
    // The operator refers to monitor numbers exactly as Windows Settings does:
    //   Raw -> DISPLAY3, Overlay -> DISPLAY2, Telemetry -> DISPLAY1.
    constexpr std::array<int, 3> roleToWindowsDisplay{3, 2, 1};

#ifdef _WIN32
    if (logicalRole < roleToWindowsDisplay.size()) {
        const int displayNumber = roleToWindowsDisplay[logicalRole];
        const auto monitor =
            windowsMonitorByDisplayNumber(displayNumber);
        if (!monitor.has_value()) {
            return false;
        }

        const RECT bounds = monitor->bounds;
        return placeBorderlessWindow(
            window,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top);
    }
#endif

    // Portable fallback: use SDL enumeration order. This path is only for
    // non-Windows platforms; Windows always uses the explicit DISPLAYn mapping
    // above so SDL ordering cannot silently swap operator displays.
    return placeWindowFullscreenOnDisplay(window, logicalRole);
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
    return placeBorderlessWindow(
        window,
        display.x,
        display.y,
        display.width,
        display.height);
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

        // A window merely changing its current display/focus is not a physical
        // topology change and must not cause all three borderless windows to be
        // repositioned/recreated. Only actual display topology events trigger
        // remapping.
        if (event.type ==
                SDL_EVENT_DISPLAY_ADDED ||
            event.type ==
                SDL_EVENT_DISPLAY_REMOVED ||
            event.type ==
                SDL_EVENT_DISPLAY_MOVED) {
            displayTopologyChanged_ = true;
        }
    }

    return false;
}

} // namespace reg::platform
