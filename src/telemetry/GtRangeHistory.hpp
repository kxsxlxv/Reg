#pragma once

#include "media/GtSei.hpp"
#include "metadata/FrameMetadata.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace reg::telemetry {

// A view-only best-effort join. Authoritative CV metrics must be measured on the
// originating devices; missing GT frames are never interpreted as missed detections.
struct GtRangePoint {
    std::chrono::steady_clock::time_point time{};
    media::FrameKey frame{};
    std::uint64_t sourceTimeNs{};
    double meters{};
};

struct GtCvMatch {
    std::chrono::steady_clock::time_point time{};
    media::FrameKey frame{};
    std::uint64_t sourceTimeNs{};
    std::uint64_t trackId{};
    std::uint16_t flags{};
    std::uint16_t classId{};
    double meters{};
    double iou{};
    float confidence{};
};

struct GtRangeTrack {
    std::string id;
    std::chrono::steady_clock::time_point latestAt{};
    double latestMeters{};
    std::deque<GtRangePoint> points;
    std::deque<GtCvMatch> matches;
};

class GtRangeHistory final {
public:
    using Clock = std::chrono::steady_clock;
    explicit GtRangeHistory(
        std::chrono::seconds retention = std::chrono::minutes(5))
        : retention_(retention) {}

    void clear() {
        tracks_.clear();
        pending_.clear();
    }

    void observeGt(const media::GtMetadata& gt, Clock::time_point when) {
        // GtMetadata must already have been validated against RGF1. Invalid,
        // absent and busy SEI do not create samples, including zero samples.
        if (gt.status != media::GtStatus::Ready)
            return;

        auto& frame = pending_[gt.source.key];
        if (frame.gtSeen)
            return;
        frame.gtSeen = true;
        frame.firstSeen = when;
        frame.sourceTimeNs = gt.source.sourceTimeNs;

        for (const auto& object : gt.objects) {
            if (!object.visible || object.id.empty())
                continue;
            const bool hasRange = object.validRange &&
                std::isfinite(object.rangeM) && object.rangeM >= 0.0;
            if (object.validBox) {
                frame.objects.push_back(Candidate{
                    object.id, object.xyxyVideo, gt.videoWidth, gt.videoHeight,
                    hasRange, object.rangeM
                });
            }
            if (!hasRange)
                continue;
            auto& track = tracks_[object.id];
            track.id = object.id;
            // Accept nondecreasing source frame time only; old packet arrivals
            // must not rewind a live chart.
            if (!track.points.empty() && when < track.latestAt)
                continue;
            track.latestAt = when;
            track.latestMeters = object.rangeM;
            // Keep visualization bounded at roughly 20 Hz per object; every
            // exact-frame CV match below retains its precise GT range.
            if (track.points.empty() ||
                when - track.points.back().time >= std::chrono::milliseconds(50)) {
                track.points.push_back(GtRangePoint{
                    when, gt.source.key, gt.source.sourceTimeNs, object.rangeM});
                if (track.points.size() > 6000U)
                    track.points.pop_front();
            }
        }

        if (frame.cvSeen)
            matchAndRemove(gt.source.key, frame);
        prune(when);
    }

    void observeCv(const metadata::FrameMetadata& cv) {
        const auto when = cv.receivedAt == Clock::time_point{}
            ? Clock::now() : cv.receivedAt;
        auto& frame = pending_[cv.key];
        if (frame.cvSeen)
            return;
        frame.cvSeen = true;
        if (frame.firstSeen == Clock::time_point{})
            frame.firstSeen = when;
        frame.targets = cv.targets;
        if (frame.gtSeen)
            matchAndRemove(cv.key, frame);
        prune(when);
    }

    std::vector<GtRangeTrack> snapshot(Clock::time_point now) const {
        std::vector<GtRangeTrack> result;
        for (const auto& [id, track] : tracks_) {
            if (now >= track.latestAt &&
                now - track.latestAt <= std::chrono::seconds(6))
                result.push_back(track);
        }
        return result;
    }

    void setMinimumIou(double threshold) noexcept {
        if (std::isfinite(threshold))
            minimumIou_ = std::clamp(threshold, 0.0, 1.0);
    }

private:
    struct Candidate {
        std::string id;
        std::array<double, 4> box{};
        std::uint32_t width{};
        std::uint32_t height{};
        bool hasRange{};
        double rangeM{};
    };

    struct Pending {
        bool gtSeen{};
        bool cvSeen{};
        Clock::time_point firstSeen{};
        std::uint64_t sourceTimeNs{};
        std::vector<Candidate> objects;
        std::vector<metadata::TargetMetadata> targets;
    };

    struct Pair {
        std::size_t gt{};
        std::size_t cv{};
        double overlap{};
    };

    static double overlap(const Candidate& gt,
                          const metadata::NormalizedRect& cv) noexcept {
        if (gt.width == 0 || gt.height == 0 ||
            !std::isfinite(cv.x) || !std::isfinite(cv.y) ||
            !std::isfinite(cv.width) || !std::isfinite(cv.height) ||
            cv.width <= 0 || cv.height <= 0)
            return 0.0;
        const double ax0 = gt.box[0] / gt.width;
        const double ay0 = gt.box[1] / gt.height;
        const double ax1 = gt.box[2] / gt.width;
        const double ay1 = gt.box[3] / gt.height;
        const double bx0 = cv.x, by0 = cv.y;
        const double bx1 = bx0 + cv.width, by1 = by0 + cv.height;
        const double w = std::max(0.0, std::min(ax1, bx1) - std::max(ax0, bx0));
        const double h = std::max(0.0, std::min(ay1, by1) - std::max(ay0, by0));
        const double intersection = w * h;
        const double total = (ax1 - ax0) * (ay1 - ay0) +
                             cv.width * cv.height - intersection;
        return total > 0.0 ? intersection / total : 0.0;
    }

    void matchAndRemove(const media::FrameKey& key, const Pending& frame) {
        std::vector<Pair> pairs;
        for (std::size_t g = 0; g < frame.objects.size(); ++g) {
            if (!frame.objects[g].hasRange)
                continue;
            for (std::size_t c = 0; c < frame.targets.size(); ++c) {
                const double iou = overlap(frame.objects[g],
                                           frame.targets[c].bbox);
                if (iou >= minimumIou_ && iou > 0.0)
                    pairs.push_back(Pair{g, c, iou});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            return a.overlap > b.overlap;
        });
        std::vector<bool> usedGt(frame.objects.size(), false);
        std::vector<bool> usedCv(frame.targets.size(), false);
        for (const auto& pair : pairs) {
            if (usedGt[pair.gt] || usedCv[pair.cv])
                continue;
            usedGt[pair.gt] = true;
            usedCv[pair.cv] = true;
            const auto& gt = frame.objects[pair.gt];
            const auto& cv = frame.targets[pair.cv];
            auto it = tracks_.find(gt.id);
            if (it == tracks_.end())
                continue;
            // A matched marker is a GT measurement at the exact CVM1 FrameKey.
            it->second.matches.push_back(GtCvMatch{
                frame.firstSeen, key, frame.sourceTimeNs, cv.id, cv.flags,
                cv.classId, gt.rangeM, pair.overlap, cv.confidence});
            if (it->second.matches.size() > 6000U)
                it->second.matches.pop_front();
        }
        pending_.erase(key);
    }

    void prune(Clock::time_point now) {
        const auto earliest = now - retention_;
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            auto& track = it->second;
            while (!track.points.empty() && track.points.front().time < earliest)
                track.points.pop_front();
            while (!track.matches.empty() && track.matches.front().time < earliest)
                track.matches.pop_front();
            if (track.latestAt < earliest)
                it = tracks_.erase(it);
            else
                ++it;
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now >= it->second.firstSeen &&
                now - it->second.firstSeen > std::chrono::seconds(3))
                it = pending_.erase(it);
            else
                ++it;
        }
        // Bounded even with a stream of CV-only frames.
        while (pending_.size() > 256) {
            const auto oldest = std::min_element(
                pending_.begin(), pending_.end(),
                [](const auto& a, const auto& b) {
                    return a.second.firstSeen < b.second.firstSeen;
                });
            pending_.erase(oldest);
        }
    }

    std::chrono::seconds retention_;
    double minimumIou_{0.30};
    std::map<std::string, GtRangeTrack> tracks_;
    std::map<media::FrameKey, Pending> pending_;
};

} // namespace reg::telemetry
