#include "launcher/WindowTiling.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

#include "launcher/LauncherDisplay.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace reg::launcher {
namespace {

HWND launcherHandle(SDL_Window* window) {
    if (!window) return nullptr;
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                               SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

std::string toUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        result.data(), length, nullptr, nullptr) <= 0) return {};
    return result;
}

std::wstring windowTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0 || length > 2048) return {};
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    const int count = GetWindowTextW(hwnd, value.data(), length + 1);
    if (count <= 0) return {};
    value.resize(static_cast<std::size_t>(count));
    return value;
}

std::wstring windowClass(HWND hwnd) {
    std::array<wchar_t, 256> buffer{};
    const int count = GetClassNameW(hwnd, buffer.data(),
                                    static_cast<int>(buffer.size()));
    return count > 0 ? std::wstring(buffer.data(), count) : std::wstring{};
}

std::wstring processImagePath(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pid);
    if (!process) return {};
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    const BOOL success = QueryFullProcessImageNameW(
        process, 0, image.data(), &length);
    CloseHandle(process);
    if (!success || length == 0) return {};
    image.resize(length);
    return image;
}

std::string applicationName(std::wstring_view image) {
    if (image.empty()) return {};
    const auto slash = image.find_last_of(L"\\/");
    const auto file = image.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
    return toUtf8(file);
}

struct EnumerationState {
    DWORD ownPid{};
    std::vector<WindowChoice> windows;
};

BOOL CALLBACK enumerateWindow(HWND hwnd, LPARAM context) {
    auto& state = *reinterpret_cast<EnumerationState*>(context);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_EX_TOOLWINDOW) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == state.ownPid) return TRUE;

    const std::wstring title = windowTitle(hwnd);
    if (title.empty()) return TRUE;
    const std::wstring className = windowClass(hwnd);
    if (className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" ||
        className == L"Shell_SecondaryTrayWnd") return TRUE;

    const auto image = processImagePath(pid);
    auto label = applicationName(image);
    if (label.empty()) label = toUtf8(className);
    const auto readableTitle = toUtf8(title);
    if (readableTitle.empty()) return TRUE;
    state.windows.push_back({
        reinterpret_cast<std::uintptr_t>(hwnd),
        static_cast<std::uint32_t>(pid),
        std::move(label),
        readableTitle,
        toUtf8(image),
        toUtf8(className)
    });
    // No need to enumerate thousands of windows: limit the picker to 256.
    return state.windows.size() >= 256 ? FALSE : TRUE;
}

RECT targetWorkArea(HWND launcher, int displayNumber) {
    HMONITOR monitor = nullptr;
    if (displayNumber > 0) {
        for (const auto& display : launcherDisplays()) {
            if (display.number != displayNumber) continue;
            const POINT center{display.x + display.width / 2,
                               display.y + display.height / 2};
            monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
            break;
        }
    } else {
        monitor = MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST);
    }
    if (!monitor) return RECT{};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) ? info.rcWork : RECT{};
}

bool positionWindow(HWND hwnd, int x, int y, int width, int height) {
    if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    return SetWindowPos(hwnd, nullptr, x, y, width, height,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

} // namespace

std::vector<WindowChoice> openApplicationWindows() {
    EnumerationState state{.ownPid = GetCurrentProcessId()};
    EnumWindows(enumerateWindow, reinterpret_cast<LPARAM>(&state));
    std::stable_sort(state.windows.begin(), state.windows.end(),
        [](const WindowChoice& lhs, const WindowChoice& rhs) {
            if (lhs.application == rhs.application) return lhs.title < rhs.title;
            return lhs.application < rhs.application;
        });
    return state.windows;
}

WindowTileResult tileWithWindow(SDL_Window* launcher,
                                int displayNumber,
                                const WindowTarget& selected,
                                std::uintptr_t preferredHandle) {
    const HWND own = launcherHandle(launcher);
    if (!own || !IsWindow(own)) return WindowTileResult::invalidWindow;
    if (selected.empty()) return WindowTileResult::noSelection;

    const auto windows = openApplicationWindows();
    const int index = findTargetIndex(windows, selected, preferredHandle);
    if (index < 0) return WindowTileResult::notRunning;
    const HWND other = reinterpret_cast<HWND>(windows[static_cast<std::size_t>(index)].handle);
    if (!IsWindow(other) || other == own)
        return WindowTileResult::notRunning;

    const RECT area = targetWorkArea(own, displayNumber);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width < 720 || height < 1100)
        return WindowTileResult::noMonitor;

    WINDOWPLACEMENT oldOwn{};
    oldOwn.length = sizeof(oldOwn);
    WINDOWPLACEMENT oldOther{};
    oldOther.length = sizeof(oldOther);
    if (!GetWindowPlacement(own, &oldOwn) ||
        !GetWindowPlacement(other, &oldOther))
        return WindowTileResult::failed;

    const int topHeight = height / 2;
    const int lowerHeight = height - topHeight;
    // The size is in screen coordinates; do not rely on Windows 11 Snap.
    if (!SDL_SetWindowMinimumSize(launcher, 600, 520))
        return WindowTileResult::failed;
    if (!positionWindow(own, area.left, area.top, width, topHeight))
        return WindowTileResult::failed;
    if (!positionWindow(other, area.left, area.top + topHeight,
                        width, lowerHeight)) {
        // A process may be elevated or reject window movement. Restore the
        // launcher instead of leaving an accidental half-finished layout.
        SetWindowPlacement(own, &oldOwn);
        SetWindowPlacement(other, &oldOther);
        return WindowTileResult::failed;
    }
    return WindowTileResult::success;
}

} // namespace reg::launcher

#else

namespace reg::launcher {
std::vector<WindowChoice> openApplicationWindows() { return {}; }
WindowTileResult tileWithWindow(SDL_Window*, int, const WindowTarget&,
                                std::uintptr_t) {
    return WindowTileResult::noMonitor;
}
} // namespace reg::launcher

#endif
