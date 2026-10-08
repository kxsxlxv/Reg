#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace reg::launcher {

// A saved target describes an application window, not an HWND: HWNDs are
// transient and are unsafe to persist between Windows sessions.
struct WindowTarget {
    std::string executablePath;
    std::string windowClass;
    std::string title;

    bool empty() const {
        return executablePath.empty() && (windowClass.empty() || title.empty());
    }
};

struct WindowChoice {
    std::uintptr_t handle{};
    std::uint32_t processId{};
    std::string application;
    std::string title;
    std::string executablePath;
    std::string windowClass;
};

inline bool caseInsensitiveEqual(std::string_view lhs,
                                 std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

inline WindowTarget targetOf(const WindowChoice& choice) {
    return {choice.executablePath, choice.windowClass, choice.title};
}

inline bool matchesTarget(const WindowChoice& choice,
                          const WindowTarget& target) {
    if (target.empty()) return false;
    if (!target.executablePath.empty()) {
        if (!caseInsensitiveEqual(choice.executablePath, target.executablePath))
            return false;
        // Group by process AND class, so a browser's main window won't
        // accidentally select an unrelated popup/helper window.
        return target.windowClass.empty() ||
               caseInsensitiveEqual(choice.windowClass, target.windowClass);
    }
    // Fallback when the executable cannot be queried: exact title and class
    // rather than a broad substring match that can grab the wrong program.
    return caseInsensitiveEqual(choice.windowClass, target.windowClass) &&
           caseInsensitiveEqual(choice.title, target.title);
}

inline int findTargetIndex(const std::vector<WindowChoice>& windows,
                           const WindowTarget& target,
                           std::uintptr_t preferredHandle = 0) {
    if (target.empty()) return -1;
    int firstMatch = -1;
    int totalMatches = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (!matchesTarget(windows[i], target)) continue;
        if (preferredHandle && windows[i].handle == preferredHandle)
            return static_cast<int>(i);
        if (caseInsensitiveEqual(windows[i].title, target.title))
            return static_cast<int>(i);
        if (firstMatch < 0) firstMatch = static_cast<int>(i);
        ++totalMatches;
    }
    // Ambiguous: never move some other window of the same application
    // simply because a title changed after the last run.
    return totalMatches == 1 ? firstMatch : -1;
}

} // namespace reg::launcher
