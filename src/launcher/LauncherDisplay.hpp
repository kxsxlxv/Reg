#pragma once
#include <vector>

struct SDL_Window;

namespace reg::launcher {
struct LauncherDisplay {
    int number{};
    int x{};
    int y{};
    int width{};
    int height{};
};

// On Windows the number is the number shown by Settings > Display > Identify.
std::vector<LauncherDisplay> launcherDisplays();
// Return false for an unavailable selection. Never leave an off-screen window.
bool moveLauncherToDisplay(SDL_Window* window, int settingsNumber);
} // namespace reg::launcher
