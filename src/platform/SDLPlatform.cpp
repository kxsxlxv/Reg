#include "app/RuntimeUiState.hpp"
#include "platform/SDLPlatform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace reg::platform {
namespace {

const char* localizedWindowTitle(const char* title) noexcept {
    if (title == nullptr) {
        return "";
    }

    const std::string_view value{title};
    if (value == "Reg - Raw") {
        return "Reg — Raw";
    }
    if (value == "Reg - Exact CV Overlay") {
        return "Reg — CV Overlay (точный)";
    }
    if (value == "Reg - Telemetry") {
        return "Reg — Телеметрия";
    }
    if (value == "Reg Replay - Raw") {
        return "Reg Replay — Raw";
    }
    if (value == "Reg Replay - Exact Overlay") {
        return "Reg Replay — Overlay (точный)";
    }
    return title;
}

const char* tracedWindowEventName(std::uint32_t type) noexcept {
    switch (type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "FOCUS_GAINED";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "FOCUS_LOST";
    case SDL_EVENT_WINDOW_EXPOSED:
        return "EXPOSED";
    case SDL_EVENT_WINDOW_RESIZED:
        return "RESIZED";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        return "PIXEL_SIZE_CHANGED";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        return "DISPLAY_CHANGED";
    default:
        return nullptr;
    }
}

void traceWindowEvent(const SDL_Event& event) {
    const char* name = tracedWindowEventName(event.type);
    if (name == nullptr) {
        return;
    }

    SDL_Window* window =
        SDL_GetWindowFromID(event.window.windowID);
    const char* title =
        window != nullptr
            ? SDL_GetWindowTitle(window)
            : nullptr;
    const SDL_WindowFlags flags =
        window != nullptr
            ? SDL_GetWindowFlags(window)
            : 0U;

    std::cerr
        << "[window-event] "
        << name
        << " id="
        << event.window.windowID
        << " title=\""
        << (title != nullptr ? title : "<unknown>")
        << "\" data="
        << event.window.data1
        << 'x'
        << event.window.data2
        << " flags=0x"
        << std::hex
        << static_cast<unsigned long long>(flags)
        << std::dec
        << '\n';
}

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
    // On Windows a real fullscreen state can cause WSI/swapchain transitions.
    // Focus ownership is handled independently: the CV output stays
    // non-focusable, while Telemetry is intentionally interactive.
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
struct WindowsSettingsDisplay final {
    int settingsNumber{};
    int x{};
    int y{};
    int width{};
    int height{};
};

struct MonitorRectLookup final {
    std::wstring deviceName;
    std::optional<RECT> rect;
};

BOOL CALLBACK monitorRectCallback(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM userData) {
    auto* lookup =
        reinterpret_cast<MonitorRectLookup*>(userData);
    if (lookup == nullptr) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(
            monitor,
            reinterpret_cast<MONITORINFO*>(&info))) {
        return TRUE;
    }

    if (lstrcmpiW(
            info.szDevice,
            lookup->deviceName.c_str()) != 0) {
        return TRUE;
    }

    lookup->rect = info.rcMonitor;
    return FALSE;
}

std::optional<RECT> desktopMonitorRectForPath(
    const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
    sourceName.header.type =
        DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;

    if (DisplayConfigGetDeviceInfo(
            &sourceName.header) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    MonitorRectLookup lookup{
        .deviceName = sourceName.viewGdiDeviceName,
        .rect = std::nullopt,
    };

    static_cast<void>(
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            monitorRectCallback,
            reinterpret_cast<LPARAM>(&lookup)));

    return lookup.rect;
}

std::vector<WindowsSettingsDisplay> windowsSettingsDisplays() {
    UINT32 pathCount = 0U;
    UINT32 modeCount = 0U;

    LONG result = GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        &modeCount);
    if (result != ERROR_SUCCESS) {
        return {};
    }

    // The topology can change between the size query and QueryDisplayConfig.
    // Retry on ERROR_INSUFFICIENT_BUFFER instead of using stale array sizes.
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

        UINT32 actualPathCount = pathCount;
        UINT32 actualModeCount = modeCount;
        result = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &actualPathCount,
            paths.data(),
            &actualModeCount,
            modes.data(),
            nullptr);

        if (result == ERROR_INSUFFICIENT_BUFFER) {
            result = GetDisplayConfigBufferSizes(
                QDC_ONLY_ACTIVE_PATHS,
                &pathCount,
                &modeCount);
            if (result != ERROR_SUCCESS) {
                return {};
            }
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return {};
        }

        paths.resize(actualPathCount);
        modes.resize(actualModeCount);

        std::vector<WindowsSettingsDisplay> displays;
        displays.reserve(paths.size());

        // Windows Settings "Identify" numbering follows the active CCD path
        // order: paths[0] -> display 1, paths[1] -> display 2, ... . Do not use
        // SDL enumeration or the GDI \\.\DISPLAYn suffix for numbering.
        //
        // The GDI monitor rectangle is still used for geometry after a path is
        // identified. It is expressed in the actual desktop coordinate space
        // and therefore already contains the effective portrait/landscape
        // orientation. Using DISPLAYCONFIG_SOURCE_MODE width/height directly
        // can describe the source surface rather than the rotated desktop
        // rectangle on a portrait target.
        for (std::size_t index = 0U; index < paths.size(); ++index) {
            const auto& path = paths[index];
            const UINT32 modeIndex = path.sourceInfo.modeInfoIdx;
            if (modeIndex == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                modeIndex >= modes.size()) {
                continue;
            }

            const auto& mode = modes[modeIndex];
            if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                continue;
            }

            const auto monitorRect =
                desktopMonitorRectForPath(path);
            if (monitorRect.has_value()) {
                displays.push_back(
                    WindowsSettingsDisplay{
                        .settingsNumber = static_cast<int>(index + 1U),
                        .x = monitorRect->left,
                        .y = monitorRect->top,
                        .width = monitorRect->right - monitorRect->left,
                        .height = monitorRect->bottom - monitorRect->top,
                    });
                continue;
            }

            // Conservative fallback for unusual display drivers where the CCD
            // source cannot be mapped back to an HMONITOR.
            const auto& source = mode.sourceMode;
            displays.push_back(
                WindowsSettingsDisplay{
                    .settingsNumber = static_cast<int>(index + 1U),
                    .x = source.position.x,
                    .y = source.position.y,
                    .width = static_cast<int>(source.width),
                    .height = static_cast<int>(source.height),
                });
        }
        return displays;
    }

    return {};
}

std::optional<WindowsSettingsDisplay>
windowsSettingsDisplayByNumber(int settingsNumber) {
    const auto displays = windowsSettingsDisplays();
    const auto it = std::ranges::find_if(
        displays,
        [settingsNumber](const WindowsSettingsDisplay& display) {
            return display.settingsNumber == settingsNumber;
        });
    if (it == displays.end()) {
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
    int height,
    bool focusable) {
    // Telemetry is an operator-facing interactive surface. Keep the legacy
    // call sites source-compatible, but never mark this role non-focusable.
    const bool effectiveFocusable =
        focusable ||
        (title != nullptr &&
         std::string_view(title) == "Reg - Telemetry");

    SDL_WindowFlags flags =
        SDL_WINDOW_VULKAN |
        SDL_WINDOW_RESIZABLE;

    if (!effectiveFocusable) {
        flags |= SDL_WINDOW_NOT_FOCUSABLE;
    }

    SDL_Window* window = SDL_CreateWindow(
        localizedWindowTitle(title),
        width,
        height,
        flags);

    if (window == nullptr) {
        throw std::runtime_error(
            std::string("SDL_CreateWindow failed: ") +
            SDL_GetError());
    }

    if (!effectiveFocusable &&
        !SDL_SetWindowFocusable(window, false)) {
        const std::string error = SDL_GetError();
        SDL_DestroyWindow(window);
        throw std::runtime_error(
            std::string("SDL_SetWindowFocusable(false) failed: ") +
            error);
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
    // These numbers are the numbers shown by Windows Settings -> Display ->
    // Identify, not SDL ordinals and not GDI \\.\DISPLAYn suffixes.
    constexpr std::array<int, 3> roleToWindowsSettingsDisplay{3, 2, 1};

#ifdef _WIN32
    if (logicalRole < roleToWindowsSettingsDisplay.size()) {
        const int settingsNumber =
            roleToWindowsSettingsDisplay[logicalRole];
        const auto display =
            windowsSettingsDisplayByNumber(settingsNumber);
        if (!display.has_value()) {
            return false;
        }

        return placeBorderlessWindow(
            window,
            display->x,
            display->y,
            display->width,
            display->height);
    }
#endif

    // Portable fallback: use SDL enumeration order. This path is only for
    // non-Windows platforms; Windows uses CCD path numbering above so the
    // operator-facing Settings numbers are authoritative.
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
        traceWindowEvent(event);

        if (event.type ==
                SDL_EVENT_QUIT ||
            event.type ==
                SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            return true;
        }

        if (event.type ==
                SDL_EVENT_KEY_DOWN &&
            !event.key.repeat) {
            if (event.key.key == SDLK_F10) {
                app::toggleTelemetryPage();
                std::cout
                    << "[telemetry] page="
                    << (app::telemetryPage() == app::TelemetryPage::NetImgui
                            ? "NetImgui"
                            : "Overview")
                    << '\n';
            } else if (event.key.key == SDLK_F11) {
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
