#pragma once

#include "telemetry/GtRangeHistory.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace reg::render {

// Visualization-only component. No frame decoding, synchronization, network
// I/O or estimator state is coupled to ImPlot.
class GtRangeDashboard final {
public:
    GtRangeDashboard();
    ~GtRangeDashboard();
    GtRangeDashboard(const GtRangeDashboard&) = delete;
    GtRangeDashboard& operator=(const GtRangeDashboard&) = delete;

    // Uses the entire remaining Overview height. No vertical scrolling: if
    // too many objects are visible, horizontal paging exposes the rest.
    void draw(const std::vector<telemetry::GtRangeTrack>& tracks,
              std::chrono::steady_clock::time_point anchor,
              bool paused);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
