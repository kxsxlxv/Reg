#include "metadata/TrackHistory.hpp"

#include <stdexcept>

namespace reg::metadata {

TrackHistory::TrackHistory(
    std::chrono::milliseconds maxAge,
    std::size_t maxPointsPerTarget)
    : maxAge_(maxAge),
      maxPointsPerTarget_(maxPointsPerTarget) {
    if (maxAge_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("TrackHistory max age must not be negative");
    }
    if (maxPointsPerTarget_ == 0) {
        throw std::invalid_argument("TrackHistory max points must be greater than zero");
    }
}

void TrackHistory::update(
    const FrameMetadata& metadata,
    std::chrono::steady_clock::time_point now) {
    prune(now);

    for (const TargetMetadata& target : metadata.targets) {
        auto& track = tracks_[target.id];

        track.push_back(TrackPoint{
            .time = now,
            .normalizedPosition = video::Vec2{
                .x = target.bbox.x + target.bbox.width * 0.5F,
                .y = target.bbox.y + target.bbox.height * 0.5F,
            },
        });

        while (track.size() > maxPointsPerTarget_) {
            track.pop_front();
        }
    }
}

std::vector<TrackPoint> TrackHistory::points(
    std::uint64_t targetId) const {
    const auto it = tracks_.find(targetId);
    if (it == tracks_.end()) {
        return {};
    }

    return std::vector<TrackPoint>(
        it->second.begin(),
        it->second.end());
}

void TrackHistory::clear() {
    tracks_.clear();
}

void TrackHistory::prune(
    std::chrono::steady_clock::time_point now) {
    const auto cutoff = now - maxAge_;

    for (auto it = tracks_.begin(); it != tracks_.end();) {
        auto& track = it->second;

        while (!track.empty() && track.front().time < cutoff) {
            track.pop_front();
        }

        if (track.empty()) {
            it = tracks_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace reg::metadata
