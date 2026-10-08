#include "launcher/MissionPlannerTile.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "launcher/LauncherDisplay.hpp"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace reg::launcher {
namespace {

HWND launcherHandle(SDL_Window* window) {
    if (!window) return nullptr;
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                               SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

struct MissionPlannerSearch {
    DWORD currentPid{};
    HWND selected{};
};

// Prefer the visible, unowned main window (not its settings dialogs).
BOOL CALLBACK findMissionPlanner(HWND hwnd, LPARAM opaque) {
    auto& state = *reinterpret_cast<MissionPlannerSearch*>(opaque);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == state.currentPid) return TRUE;

    const int length = GetWindowTextLengthW(hwnd);
    if (length < 14 || length > 512) return TRUE;
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int written = GetWindowTextW(hwnd, title.data(), length + 1);
    if (written <= 0) return TRUE;
    title.resize(static_cast<std::size_t>(written));
    std::transform(title.begin(), title.end(), title.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    if (title.find(L"mission planner") == std::wstring::npos) return TRUE;

    state.selected = hwnd;
    return FALSE;
}

RECT targetWorkArea(HWND launcher, int displayNumber) {
    HMONITOR monitor = nullptr;
    if (displayNumber > 0) {
        for (const auto& display : launcherDisplays()) {
            if (display.number != displayNumber) continue;
            POINT center{
                display.x + display.width / 2,
                display.y + display.height / 2
            };
            monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
            break;
        }
    } else {
        monitor = MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST);
    }
    if (!monitor) return RECT{};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return RECT{};
    return info.rcWork; // excludes Windows taskbars, unlike rcMonitor
}

bool positionWindow(HWND hwnd, int x, int y, int width, int height) {
    // Restore maximized/minimized windows before choosing their position.
    if (IsIconic(hwnd) || IsZoomed(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    return SetWindowPos(hwnd, nullptr, x, y, width, height,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

} // namespace

MissionPlannerTileResult tileWithMissionPlanner(
    SDL_Window* launcher, int displayNumber) {
    HWND own = launcherHandle(launcher);
    if (!own || !IsWindow(own))
        return MissionPlannerTileResult::invalidWindow;

    MissionPlannerSearch search{.currentPid = GetCurrentProcessId()};
    EnumWindows(findMissionPlanner, reinterpret_cast<LPARAM>(&search));
    if (!search.selected || !IsWindow(search.selected))
        return MissionPlannerTileResult::notFound;

    const RECT area = targetWorkArea(own, displayNumber);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    // Give both applications enough vertical room for their controls.
    if (width < 720 || height < 1100)
        return MissionPlannerTileResult::noMonitor;

    const int topHeight = height / 2;
    const int lowerHeight = height - topHeight;

    // A minimum SDL window size may otherwise prevent exact half-height.
    if (!SDL_SetWindowMinimumSize(launcher, 600, 520))
        return MissionPlannerTileResult::failed;
    if (!positionWindow(own, area.left, area.top, width, topHeight))
        return MissionPlannerTileResult::failed;
    if (!positionWindow(search.selected,
                        area.left, area.top + topHeight,
                        width, lowerHeight))
        return MissionPlannerTileResult::failed;
    return MissionPlannerTileResult::success;
}

} // namespace reg::launcher

#else

namespace reg::launcher {
MissionPlannerTileResult tileWithMissionPlanner(SDL_Window*, int) {
    return MissionPlannerTileResult::noMonitor;
}
} // namespace reg::launcher

#endif
