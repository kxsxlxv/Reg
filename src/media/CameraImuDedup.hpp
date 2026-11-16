#pragma once

#include "media/GtSei.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>

namespace reg::media {

// One consumer owns its own bounded dedupe lifetime. The stream/source
// generation is supplied by the caller (reset/reconnect = new domain).
// Repeated per-frame IMU windows are compared by sensor+sequence and contents;
// timestamps alone never distinguish video frames or sample identities.
class CameraImuDedup final {
public:
    explicit CameraImuDedup(std::size_t capacity = 4096U)
        : capacity_(capacity == 0U ? 1U : capacity) {}

    struct Stats {
        std::uint64_t accepted{};
        std::uint64_t duplicates{};
        std::uint64_t conflicts{};
        std::uint64_t evicted{};
        std::uint64_t sessions{};
    };

    template<class F>
    void observe(const CameraImuMetadata& imu,F&& onNew,
                 std::uint64_t sourceGeneration=0U) {
        if (imu.status!=CameraImuStatus::Ready) return;
        if (runId_!=imu.runId || simulationEpoch_!=imu.simulationEpoch ||
            sourceGeneration_!=sourceGeneration) {
            runId_=imu.runId;
            simulationEpoch_=imu.simulationEpoch;
            sourceGeneration_=sourceGeneration;
            window_.clear();
            seen_.clear();
            ++stats_.sessions;
        }
        for(const auto& sample:imu.imu) {
            Key key{sample.sensorId,sample.sequence};
            if (const auto found=seen_.find(key);found!=seen_.end()) {
                if(same(found->second,sample)) ++stats_.duplicates;
                else ++stats_.conflicts;
                continue;
            }
            seen_.emplace(key,sample);
            window_.push_back(std::move(key));
            ++stats_.accepted;
            onNew(sample);
            if(window_.size()>capacity_) {
                seen_.erase(window_.front());
                window_.pop_front();
                ++stats_.evicted;
            }
        }
    }
    Stats stats() const noexcept {return stats_;}
    void reset() noexcept {
        runId_.clear();simulationEpoch_=0;sourceGeneration_=0;
        window_.clear();seen_.clear();stats_={};
    }

private:
    struct Key {
        std::string sensorId;
        std::uint64_t sequence{};
        bool operator==(const Key& other) const noexcept {
            return sensorId==other.sensorId && sequence==other.sequence;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            const auto a=std::hash<std::string>{}(k.sensorId);
            const auto b=std::hash<std::uint64_t>{}(k.sequence);
            return a^(b+0x9e3779b9U+(a<<6U)+(a>>2U));
        }
    };
    static bool same(const CameraImuSample& a,
                     const CameraImuSample& b) noexcept {
        return a.timestampNs==b.timestampNs &&
               a.intervalStartNs==b.intervalStartNs &&
               a.sequence==b.sequence &&
               a.sensorId==b.sensorId &&
               a.statusFlags==b.statusFlags &&
               a.gyroRadS==b.gyroRadS &&
               a.accelMS2==b.accelMS2;
    }

    std::size_t capacity_{};
    std::string runId_;
    std::uint64_t simulationEpoch_{};
    std::uint64_t sourceGeneration_{};
    std::deque<Key> window_;
    std::unordered_map<Key,CameraImuSample,KeyHash> seen_;
    Stats stats_{};
};

} // namespace reg::media
