#pragma once

#include "metadata/FrameMetadata.hpp"
#include "video/VideoTransform.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace reg::metadata {

struct TrackPoint {
    std::chrono::steady_clock::time_point time{};
    video::Vec2 normalizedPosition{};
};

class TrackHistory final {
public:
    explicit TrackHistory(
        std::chrono::milliseconds maxAge = std::chrono::seconds{5},
        std::size_t maxPointsPerTarget = 1024);

    void update(
        const FrameMetadata& metadata,
        std::chrono::steady_clock::time_point now);

    std::vector<TrackPoint> points(std::uint64_t targetId) const;

    void clear();
    std::size_t targetCount() const noexcept { return tracks_.size(); }

private:
    void prune(std::chrono::steady_clock::time_point now);

    std::chrono::milliseconds maxAge_;
    std::size_t maxPointsPerTarget_{};
    std::unordered_map<std::uint64_t, std::deque<TrackPoint>> tracks_;
};

} // namespace reg::metadata
