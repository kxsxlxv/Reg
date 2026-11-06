#pragma once

#include <algorithm>
#include <vector>

namespace reg::platform {

// Pure selection policy shared by Windows CCD and portable SDL placement.
// An absent requested monitor, or one temporarily reserved for Jetson,
// falls back to the first active Windows-owned monitor. Zero means there is
// currently no safe output; never silently redirect to the reserved display.
inline int selectUsableDisplay(
    int requested,
    int reserved,
    const std::vector<int>& active) {
    const auto usable = [&](int number) {
        return number > 0 && number != reserved &&
            std::find(active.begin(), active.end(), number) != active.end();
    };
    if (usable(requested)) return requested;
    for (const int number : active) {
        if (usable(number)) return number;
    }
    return 0;
}

} // namespace reg::platform
