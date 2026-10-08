#include "launcher/LauncherLayout.hpp"

#include <cassert>

int main() {
    using reg::launcher::DashboardColumns;
    using reg::launcher::dashboardColumns;
    using reg::launcher::dashboardColumnWidth;

    static_assert(dashboardColumns(1170.f) == DashboardColumns::three);
    static_assert(dashboardColumns(1080.f) == DashboardColumns::three);
    static_assert(dashboardColumns(1079.f) == DashboardColumns::two);
    static_assert(dashboardColumns(900.f) == DashboardColumns::two);
    static_assert(dashboardColumns(759.f) == DashboardColumns::one);

    // 1200-pixel portrait screens provide three practical card columns,
    // with enough room for the longest label and a 116-168px input field.
    assert(dashboardColumnWidth(1160.f, DashboardColumns::three) > 375.f);
    assert(dashboardColumnWidth(905.f, DashboardColumns::two) > 445.f);
    assert(dashboardColumnWidth(650.f, DashboardColumns::one) == 650.f);
}
