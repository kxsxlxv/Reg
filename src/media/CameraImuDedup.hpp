#pragma once

#include "media/GtSei.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>

namespace reg::media {

// Consumer-owned, single-threaded de-duplication of overlapping KGT1 IMU
// windows. run_id + sequence is the key; simulation timestamps are NOT keys.
// Bounded history; a new run_id starts a fresh sequence domain.
class CameraImuDedup final {
public:
    explicit CameraImuDedup(std::size_t capacity = 4096U)
        : capacity_(capacity == 0U ? 1U : capacity) {}

    struct Stats {
        std::uint64_t accepted{};
        std::uint64_t duplicates{};
        std::uint64_t evicted{};
    };

    template<class F>
    void observe(const CameraImuMetadata& imu, F&& onNew) {
        if (imu.status != CameraImuStatus::Ready) return;
        if (runId_ != imu.runId) {
            runId_ = imu.runId;
            window_.clear();
            seen_.clear();
        }
        for (const auto& sample : imu.imu) {
            if (!seen_.insert(sample.sequence).second) {
                ++stats_.duplicates;
                continue;
            }
            window_.push_back(sample.sequence);
            ++stats_.accepted;
            onNew(sample);
            if (window_.size() > capacity_) {
                seen_.erase(window_.front());
                window_.pop_front();
                ++stats_.evicted;
            }
        }
    }

    Stats stats() const noexcept { return stats_; }
    void reset() noexcept {
        runId_.clear();
        window_.clear();
        seen_.clear();
        stats_ = {};
    }

private:
    std::size_t capacity_{};
    std::string runId_;
    std::deque<std::uint64_t> window_;
    std::unordered_set<std::uint64_t> seen_;
    Stats stats_{};
};

} // namespace reg::media
