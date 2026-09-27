#include "overlay/TrackHistory.hpp"

#include <algorithm>
#include <stdexcept>

namespace reg::overlay {

TrackHistory::TrackHistory(std::chrono::milliseconds retention)
    : retention_(retention) {
    if (retention_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("TrackHistory retention must be positive");
    }
}

void TrackHistory::observe(
    const metadata::FrameMetadata& metadata,
    Clock::time_point presentedAt) {
    purge(presentedAt);

    for (const auto& target : metadata.targets) {
        const Vec2f center{
            target.bbox.x + target.bbox.width * 0.5F,
            target.bbox.y + target.bbox.height * 0.5F,
        };

        auto& track = tracks_[target.id];

        if (!track.empty() && track.back().key == metadata.key) {
            // Exact duplicate frame metadata should normally be filtered before
            // this layer, but keeping history idempotent makes replay safer.
            continue;
        }

        track.push_back(TrackPoint{
            .key = metadata.key,
            .presentedAt = presentedAt,
            .normalizedCenter = center,
            .classId = target.classId,
        });
    }
}

std::size_t TrackHistory::purge(Clock::time_point now) {
    std::size_t removedPoints = 0;

    for (auto trackIt = tracks_.begin(); trackIt != tracks_.end();) {
        auto& track = trackIt->second;

        while (!track.empty()) {
            const auto timestamp = track.front().presentedAt;
            if (timestamp > now || now - timestamp <= retention_) {
                break;
            }

            track.pop_front();
            ++removedPoints;
        }

        if (track.empty()) {
            trackIt = tracks_.erase(trackIt);
        } else {
            ++trackIt;
        }
    }

    return removedPoints;
}

std::vector<TrackPoint> TrackHistory::history(std::uint64_t targetId) const {
    const auto found = tracks_.find(targetId);
    if (found == tracks_.end()) {
        return {};
    }

    return std::vector<TrackPoint>(
        found->second.begin(),
        found->second.end());
}

void TrackHistory::setRetention(std::chrono::milliseconds retention) {
    if (retention <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("TrackHistory retention must be positive");
    }

    retention_ = retention;
}

void TrackHistory::clear() noexcept {
    tracks_.clear();
}

} // namespace reg::overlay
