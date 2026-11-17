#pragma once

#include "media/GtAccessUnit.hpp"
#include "media/GtMapDecode.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace reg::media {

struct GtDecodedBundle {
    SourceFrameIdentity identity{};
    GtDecodedMap sky;
    GtDecodedMap depth;
    std::chrono::steady_clock::time_point finishedAt{};
};

struct GtMapWorkerStats {
    std::uint64_t submitted{}, duplicate{}, skipped{}, queueDrops{},
                  lockDrops{}, sessionResets{}, decodedSky{}, decodedDepth{},
                  invalidSky{}, invalidDepth{}, checksumFailures{},
                  durationUs{}, longestUs{};
    std::size_t pendingBytes{}, pendingFrames{}, readyBytes{}, readyFrames{};
    std::uint64_t oldestPendingAgeMs{};
    std::uint64_t cacheEvictedAge{},cacheEvictedCapacity{};
    std::uint64_t lookupHits{},lookupMisses{},lookupBusy{};
};

// Compressed GT only: worker never retains an AVFrame or AVVkFrame lease.
// Decode-once for all panels, indexed strictly by source FrameKey.
class GtMapWorker final {
public:
    GtMapWorker();
    ~GtMapWorker();
    GtMapWorker(const GtMapWorker&)=delete;
    GtMapWorker& operator=(const GtMapWorker&)=delete;

    // Non-blocking best-effort live policy; recorder owns its own complete
    // archive path, *before* this leaky presentation worker.
    bool submit(std::shared_ptr<const FrameGtParts> parts) noexcept;
    std::shared_ptr<const GtDecodedBundle> find(
        const FrameKey& key) const noexcept;
    void reset() noexcept;
    GtMapWorkerStats stats() const noexcept;

private:
    struct Task {
        std::shared_ptr<const FrameGtParts> parts;
        std::size_t bytes{};
        std::uint64_t generation{};
        std::chrono::steady_clock::time_point enqueuedAt{};
    };
    struct Cached {
        std::shared_ptr<const GtDecodedBundle> result;
        std::size_t bytes{};
        std::chrono::steady_clock::time_point createdAt{};
    };
    void run(std::stop_token token);
    void pruneLocked(std::chrono::steady_clock::time_point now);
    static constexpr std::size_t kQueueMaxBytes=24U*1024U*1024U;
    // The AU/SEI worker now runs *before* Vulkan decode. FFmpeg's
    // hardware-decode reorder/queue can lag GT ingestion by 40+ frames.
    // A 16-frame cache (~267 ms at 60 FPS) evicted each mask before RGB
    // reached presentation. Retain >1 s of 480x270 maps, bounded by bytes.
    static constexpr std::size_t kCacheMaxFrames=128U;
    static constexpr std::size_t kCacheMaxBytes=96U*1024U*1024U;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Task> pending_;
    std::map<FrameKey,Cached> ready_;
    std::size_t pendingBytes_{},readyBytes_{};
    std::uint64_t generation_{};
    mutable std::atomic_uint64_t lookupHits_{0},lookupMisses_{0},lookupBusy_{0};
    std::atomic_uint64_t cacheEvictedAge_{0},cacheEvictedCapacity_{0};
    std::atomic_uint64_t submitted_{0},duplicate_{0},skipped_{0},queueDrops_{0},
        lockDrops_{0},sessionResets_{0},decodedSky_{0},decodedDepth_{0},
        invalidSky_{0},invalidDepth_{0},checksumFailures_{0},durationUs_{0},
        longestUs_{0};
    // Start after all state used by run() has been initialized.
    std::jthread worker_;
};

} // namespace reg::media
