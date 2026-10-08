#pragma once

namespace reg::launcher {

// Layout thresholds refer to the available ImGui content width, not the
// physical monitor resolution. Three columns need at least ~348 px each.
enum class DashboardColumns : int { one = 1, two = 2, three = 3 };

constexpr DashboardColumns dashboardColumns(float width) noexcept {
    if (width >= 1080.0f) return DashboardColumns::three;
    if (width >= 760.0f) return DashboardColumns::two;
    return DashboardColumns::one;
}

constexpr float dashboardColumnWidth(float width,
                                     DashboardColumns mode,
                                     float gap = 9.0f) noexcept {
    const auto count = static_cast<int>(mode);
    return (width - (count - 1) * gap) / count;
}
