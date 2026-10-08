#include "launcher/LauncherDisplay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace reg::launcher {
namespace {
#ifdef _WIN32
struct MonitorLookup {
    std::wstring device;
    std::optional<RECT> rect;
};

BOOL CALLBACK findMonitor(HMONITOR handle, HDC, LPRECT, LPARAM user) {
    auto* lookup = reinterpret_cast<MonitorLookup*>(user);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(handle, reinterpret_cast<MONITORINFO*>(&info)) &&
        lstrcmpiW(info.szDevice, lookup->device.c_str()) == 0) {
        lookup->rect = info.rcMonitor;
        return FALSE;
    }
    return TRUE;
}

std::optional<RECT> pathRectangle(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    MonitorLookup lookup{.device = source.viewGdiDeviceName};
    EnumDisplayMonitors(nullptr, nullptr, findMonitor,
                        reinterpret_cast<LPARAM>(&lookup));
    return lookup.rect;
}
#endif
} // namespace

std::vector<LauncherDisplay> launcherDisplays() {
#ifdef _WIN32
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
                                               &pathCount, &modeCount);
    if (status != ERROR_SUCCESS) return {};

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        UINT32 actualPaths = pathCount;
        UINT32 actualModes = modeCount;
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                                    &actualPaths, paths.data(),
                                    &actualModes, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
                                                 &pathCount, &modeCount);
            if (status != ERROR_SUCCESS) return {};
            continue;
        }
        if (status != ERROR_SUCCESS) return {};
        paths.resize(actualPaths);
        modes.resize(actualModes);
        std::vector<LauncherDisplay> result;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const auto mode = paths[i].sourceInfo.modeInfoIdx;
            if (mode == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                mode >= modes.size() ||
                modes[mode].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                continue;
            }
            if (const auto rect = pathRectangle(paths[i])) {
                result.push_back({
                    static_cast<int>(i + 1), rect->left, rect->top,
                    rect->right - rect->left, rect->bottom - rect->top});
            } else {
                const auto& source = modes[mode].sourceMode;
                result.push_back({
                    static_cast<int>(i + 1), source.position.x, source.position.y,
                    static_cast<int>(source.width), static_cast<int>(source.height)});
            }
        }
        return result;
    }
    return {};
#else
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    std::vector<LauncherDisplay> result;
    for (int i = 0; ids && i < count; ++i) {
        SDL_Rect rect{};
        if (SDL_GetDisplayBounds(ids[i], &rect)) {
            result.push_back({i + 1, rect.x, rect.y, rect.w, rect.h});
        }
    }
    if (ids) SDL_free(ids);
    return result;
#endif
}

bool moveLauncherToDisplay(SDL_Window* window, int settingsNumber) {
    if (!window || settingsNumber < 0) return false;
    LauncherDisplay chosen{};
    if (settingsNumber == 0) {
        SDL_Rect primary{};
        if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &primary))
            return false;
        chosen = {0, primary.x, primary.y, primary.w, primary.h};
    } else {
        const auto displays = launcherDisplays();
        const auto it = std::find_if(displays.begin(), displays.end(),
            [settingsNumber](const LauncherDisplay& display) {
                return display.number == settingsNumber;
            });
        if (it == displays.end()) return false;
        chosen = *it;
    }
    if (chosen.width < 320 || chosen.height < 320) return false;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    // Leave breathing room for taskbars, title bars and window shadows.
    const int targetWidth = std::min(width, std::max(320, chosen.width - 64));
    const int targetHeight = std::min(height, std::max(320, chosen.height - 100));
    if (!SDL_SetWindowSize(window, targetWidth, targetHeight)) return false;
    const int x = chosen.x + (chosen.width - targetWidth) / 2;
    const int y = chosen.y + (chosen.height - targetHeight) / 2;
    return SDL_SetWindowPosition(window, x, y);
}

} // namespace reg::launcher
