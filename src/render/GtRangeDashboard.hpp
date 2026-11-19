#pragma once

#include "telemetry/TelemetryModel.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace reg::render {

// Visualization-only component. No frame decoding, synchronization, network
// I/O or estimator state is coupled to ImPlot.
struct GtRangeViewSettings {
    bool frameAxis{false};
    bool manual{false};
    double windowSeconds{60.0};
    std::uint64_t windowFrames{600};
    double fromSeconds{-60.0};
    double toSeconds{0.0};
    std::uint64_t fromFrame{};
    std::uint64_t toFrame{};
};

class GtRangeDashboard final {
public:
    GtRangeDashboard();
    ~GtRangeDashboard();
    GtRangeDashboard(const GtRangeDashboard&) = delete;
    GtRangeDashboard& operator=(const GtRangeDashboard&) = delete;

    // Uses the entire remaining Overview height. No vertical scrolling: if
    // too many objects are visible, horizontal paging exposes the rest.
    void draw(const std::vector<telemetry::GtRangeTrack>& tracks,
              const telemetry::Counters& counters,
              const telemetry::GtCvDiagnostics& diagnostics,
              std::chrono::steady_clock::time_point anchor,
              bool paused,
              GtRangeViewSettings& view);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
