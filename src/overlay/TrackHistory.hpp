#pragma once

#include "media/FrameIdentity.hpp"
#include "metadata/Protocol.hpp"
#include "overlay/Geometry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace reg::overlay {

struct TrackPoint {
    media::FrameKey key{};
    std::chrono::steady_clock::time_point presentedAt{};
    Vec2f normalizedCenter{};
    std::uint16_t classId{};
};

class TrackHistory final {
public:
    using Clock = std::chrono::steady_clock;

    explicit TrackHistory(
        std::chrono::milliseconds retention = std::chrono::seconds(5));

    // Intended to be called on the Overlay/render thread when an exact
    // synchronized frame is accepted for presentation.
    void observe(
        const metadata::FrameMetadata& metadata,
        Clock::time_point presentedAt);

    std::size_t purge(Clock::time_point now);

    std::vector<TrackPoint> history(std::uint64_t targetId) const;

    void setRetention(std::chrono::milliseconds retention);
    std::chrono::milliseconds retention() const noexcept { return retention_; }

    void clear() noexcept;
    std::size_t trackCount() const noexcept { return tracks_.size(); }

private:
    using Track = std::deque<TrackPoint>;

    std::chrono::milliseconds retention_{};
    std::unordered_map<std::uint64_t, Track> tracks_;
};

} // namespace reg::overlay
