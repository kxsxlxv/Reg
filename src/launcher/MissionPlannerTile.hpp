#pragma once

#include <string>

struct SDL_Window;

namespace reg::launcher {

enum class MissionPlannerTileResult {
    success,
    notFound,
    noMonitor,
    invalidWindow,
    failed
};

// Explicitly tile the launcher in the upper half and the existing Mission
// Planner main window in the lower half of the same monitor. Never launches,
// kills, or embeds a third-party process.
MissionPlannerTileResult tileWithMissionPlanner(SDL_Window* launcher,
                                                int displayNumber);

} // namespace reg::launcher
