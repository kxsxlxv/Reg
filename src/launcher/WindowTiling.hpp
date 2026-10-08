#pragma once

#include "launcher/WindowIdentity.hpp"

#include <vector>

struct SDL_Window;

namespace reg::launcher {

enum class WindowTileResult {
    success,
    noSelection,
    notRunning,
    noMonitor,
    invalidWindow,
    failed
};

// Enumerate user-visible, non-owned top-level windows in the current
// interactive Windows desktop. Never returns the launcher's own process.
std::vector<WindowChoice> openApplicationWindows();

// Place Launcher in the upper 50% and the chosen *running* window in the
// lower 50% of the monitor's work area (taskbar excluded).
// preferredHandle is checked against the identity before use; never blindly
// trust a persisted HWND.
WindowTileResult tileWithWindow(SDL_Window* launcher,
                                int displayNumber,
                                const WindowTarget& selected,
                                std::uintptr_t preferredHandle = 0);

} // namespace reg::launcher
