#include "media/GtMapWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace reg::media {
namespace {
void atomicMax(std::atomic_uint64_t& slot,std::uint64_t n) noexcept {
    auto x=slot.load(std::memory_order_relaxed);
    while(n>x && !slot.compare_exchange_weak(x,n,std::memory_order_relaxed)) {}
}
std::size_t encodedBytes(const FrameGtParts& p) noexcept {
    return p.sky.encoded.size()+p.depth.encoded.size();
}
std::size_t rawBytes(const GtDecodedBundle& p) noexcept {
    return (p.sky.raw?p.sky.raw->size():0U)+
           (p.depth.raw?p.depth.raw->size():0U);
}
} // namespace

GtMapWorker::GtMapWorker()
    :worker_([this](std::stop_token token){run(token);}) {}

GtMapWorker::~GtMapWorker() {
    worker_.request_stop();
    changed_.notify_all();
    if(worker_.joinable())worker_.join();
}

bool GtMapWorker::submit(std::shared_ptr<const FrameGtParts> parts) noexcept {
    if(!parts || !parts->source ||
       (parts->sky.status!=GtMapStatus::Encoded &&
        parts->depth.status!=GtMapStatus::Encoded)) {
        skipped_.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
    try {
    const auto now=std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()) {
        lockDrops_.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
    const auto id=parts->source->key;
    for(const auto& task:pending_)
        if(task.parts->source->key==id && task.generation==generation_) {
            duplicate_.fetch_add(1U,std::memory_order_relaxed);
            return true;
        }
    if(ready_.contains(id)) {
        duplicate_.fetch_add(1U,std::memory_order_relaxed);
        return true;
    }
    const auto bytes=encodedBytes(*parts);
    if(bytes>kQueueMaxBytes) {
        queueDrops_.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
    while((pending_.size()>=32U || pendingBytes_+bytes>kQueueMaxBytes) &&
          !pending_.empty()) {
        pendingBytes_-=pending_.front().bytes;
        pending_.pop_front();
        queueDrops_.fetch_add(1U,std::memory_order_relaxed);
    }
    pending_.push_back(Task{std::move(parts),bytes,generation_,now});
    pendingBytes_+=bytes;
    submitted_.fetch_add(1U,std::memory_order_relaxed);
    changed_.notify_one();
    return true;
    } catch (...) {
        queueDrops_.fetch_add(1U,std::memory_order_relaxed);
        return false;
    }
}

std::shared_ptr<const GtDecodedBundle>
GtMapWorker::find(const FrameKey& key) const noexcept {
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()) {
        lookupBusy_.fetch_add(1U,std::memory_order_relaxed);
        return {};
    }
    const auto it=ready_.find(key);
    if(it==ready_.end()) {
        lookupMisses_.fetch_add(1U,std::memory_order_relaxed);
        return {};
    }
    lookupHits_.fetch_add(1U,std::memory_order_relaxed);
    return it->second.result;
}

void GtMapWorker::reset() noexcept {
    std::scoped_lock lock(mutex_);
    ++generation_;
    pending_.clear();ready_.clear();
    pendingBytes_=0;readyBytes_=0;
    sessionResets_.fetch_add(1U,std::memory_order_relaxed);
}

GtMapWorkerStats GtMapWorker::stats() const noexcept {
    GtMapWorkerStats result;
    result.submitted=submitted_.load(std::memory_order_relaxed);
    result.duplicate=duplicate_.load(std::memory_order_relaxed);
    result.skipped=skipped_.load(std::memory_order_relaxed);
    result.queueDrops=queueDrops_.load(std::memory_order_relaxed);
    result.lockDrops=lockDrops_.load(std::memory_order_relaxed);
    result.sessionResets=sessionResets_.load(std::memory_order_relaxed);
    result.decodedSky=decodedSky_.load(std::memory_order_relaxed);
    result.decodedDepth=decodedDepth_.load(std::memory_order_relaxed);
    result.invalidSky=invalidSky_.load(std::memory_order_relaxed);
    result.invalidDepth=invalidDepth_.load(std::memory_order_relaxed);
    result.checksumFailures=checksumFailures_.load(std::memory_order_relaxed);
    result.durationUs=durationUs_.load(std::memory_order_relaxed);
    result.longestUs=longestUs_.load(std::memory_order_relaxed);
    result.cacheEvictedAge=cacheEvictedAge_.load(std::memory_order_relaxed);
    result.cacheEvictedCapacity=cacheEvictedCapacity_.load(std::memory_order_relaxed);
    result.lookupHits=lookupHits_.load(std::memory_order_relaxed);
    result.lookupMisses=lookupMisses_.load(std::memory_order_relaxed);
    result.lookupBusy=lookupBusy_.load(std::memory_order_relaxed);
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(lock.owns_lock()) {
        result.pendingBytes=pendingBytes_;
        result.pendingFrames=pending_.size();
        result.readyBytes=readyBytes_;
        result.readyFrames=ready_.size();
        if(!pending_.empty()) {
            const auto age=std::chrono::steady_clock::now()-pending_.front().enqueuedAt;
            result.oldestPendingAgeMs=static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        }
    }
    return result;
}

void GtMapWorker::pruneLocked(std::chrono::steady_clock::time_point now) {
    for(auto it=ready_.begin();it!=ready_.end();) {
        if(now-it->second.createdAt>std::chrono::seconds(5)) {
            readyBytes_-=it->second.bytes;
            it=ready_.erase(it);
            cacheEvictedAge_.fetch_add(1U,std::memory_order_relaxed);
        } else ++it;
    }
    while((readyBytes_>kCacheMaxBytes || ready_.size()>kCacheMaxFrames) &&
          !ready_.empty()) {
        auto oldest=ready_.begin();
        for(auto it=ready_.begin();it!=ready_.end();++it)
            if(it->second.createdAt<oldest->second.createdAt)oldest=it;
        readyBytes_-=oldest->second.bytes;
        ready_.erase(oldest);
        cacheEvictedCapacity_.fetch_add(1U,std::memory_order_relaxed);
    }
}

void GtMapWorker::run(std::stop_token token) {
    while(!token.stop_requested()) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock,token,std::chrono::milliseconds(150),[&]{
                return !pending_.empty();
            });
            if(token.stop_requested())break;
            pruneLocked(std::chrono::steady_clock::now());
            if(pending_.empty())continue;
            task=std::move(pending_.front());
            pending_.pop_front();
            pendingBytes_-=task.bytes;
        }
        try {
        // No GPU frame/surface reference exists in this task.
        auto decoded=std::make_shared<GtDecodedBundle>();
        decoded->identity=*task.parts->source;
        if(task.parts->sky.status==GtMapStatus::Encoded) {
            decoded->sky=decodeGtMap(task.parts->sky);
            if(decoded->sky.status==GtMapStatus::Ready)
                decodedSky_.fetch_add(1U,std::memory_order_relaxed);
            else {
                invalidSky_.fetch_add(1U,std::memory_order_relaxed);
                if(decoded->sky.error==GtMapDecodeError::Checksum)
                    checksumFailures_.fetch_add(1U,std::memory_order_relaxed);
            }
        }
        if(task.parts->depth.status==GtMapStatus::Encoded) {
            decoded->depth=decodeGtMap(task.parts->depth);
            if(decoded->depth.status==GtMapStatus::Ready)
                decodedDepth_.fetch_add(1U,std::memory_order_relaxed);
            else {
                invalidDepth_.fetch_add(1U,std::memory_order_relaxed);
                if(decoded->depth.error==GtMapDecodeError::Checksum)
                    checksumFailures_.fetch_add(1U,std::memory_order_relaxed);
            }
        }
        const auto duration=decoded->sky.durationUs+decoded->depth.durationUs;
        durationUs_.fetch_add(duration,std::memory_order_relaxed);
        atomicMax(longestUs_,duration);
        decoded->finishedAt=std::chrono::steady_clock::now();
        std::scoped_lock lock(mutex_);
        if(task.generation!=generation_) continue;
        const auto bytes=rawBytes(*decoded);
        // Capture the key BEFORE moving decoded into the cache. Function
        // argument evaluation order is unspecified: reading decoded->identity
        // alongside std::move(decoded) caused a Windows CTest segfault.
        const FrameKey key = decoded->identity.key;
        Cached cached{std::move(decoded),bytes,std::chrono::steady_clock::now()};
        auto [it,inserted]=ready_.emplace(key,std::move(cached));
        if(inserted) readyBytes_+=bytes;
        pruneLocked(std::chrono::steady_clock::now());
        } catch (...) {
            // Out of memory in optional diagnostic/map handling cannot
            // terminate the process or the hardware decode thread.
            queueDrops_.fetch_add(1U,std::memory_order_relaxed);
        }
    }
}
} // namespace reg::media
