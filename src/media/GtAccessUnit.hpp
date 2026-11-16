#pragma once

#include "media/GtSei.hpp"
#include "media/H264AuSei.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace reg::media {

struct FrameGtParts final {
    std::optional<SourceFrameIdentity> source;
    GtMetadata bbox;
    GtMapMetadata sky{.kind=GtMapKind::Sky};
    GtMapMetadata depth{.kind=GtMapKind::Depth};
    std::uint32_t unknownSei{};
    std::uint32_t identicalDuplicates{};
    std::uint32_t conflictingFields{};
    bool malformedAu{};
    bool oversizedAu{};
};

// Owns all compressed metadata before hardware decode can lose SEI side data.
inline std::shared_ptr<const FrameGtParts> makeGtParts(H264AuSei au) {
    auto result=std::make_shared<FrameGtParts>();
    result->unknownSei=au.unknownUuidMessages;
    result->identicalDuplicates=au.byteIdenticalDuplicates;
    result->malformedAu=au.malformed;
    result->oversizedAu=au.oversized;
    for (bool c:au.conflicts) if (c) ++result->conflictingFields;
    if (au.malformed || au.oversized) return result;
    if (au.get(AuSeiKind::Rgf1)) {
        const auto& payload=*au.get(AuSeiKind::Rgf1);
        if (payload->size()==kFrameIdentityPayloadSize)
            result->source=decodeFrameIdentityPayload(*payload);
    }
    if (au.get(AuSeiKind::Kgt1)) {
        const auto& payload=**au.get(AuSeiKind::Kgt1);
        result->bbox=parseGtJson(payload);
    }
    if (au.get(AuSeiKind::Ksm1)) {
        result->sky=parseGtMapBinary(**au.get(AuSeiKind::Ksm1),
                                     GtMapKind::Sky);
    }
    if (au.get(AuSeiKind::Kdm1)) {
        result->depth=parseGtMapBinary(**au.get(AuSeiKind::Kdm1),
                                       GtMapKind::Depth);
    }
    if (au.conflicts[1]) result->bbox.status=GtStatus::ParseError;
    if (au.conflicts[2]) result->sky.status=GtMapStatus::Invalid;
    if (au.conflicts[3]) result->depth.status=GtMapStatus::Invalid;
    if (au.conflicts[0]) result->source.reset();
    validateGtMapForFrame(result->sky,result->source);
    validateGtMapForFrame(result->depth,result->source);
    return result;
}

// Single-threaded demux/decoder PTS domain. Never pop "next" metadata on frame
// output; HEVC/H264 reordering makes FIFO and latest-frame joins unsafe.
class GtPtsAssociation final {
public:
    using Clock=std::chrono::steady_clock;
    struct Stats {
        std::uint64_t inserted{}, matched{}, missingPts{}, misses{},
                      duplicatePts{}, conflictingPts{}, ageEvicted{},
                      sizeEvicted{}, resets{};
    };

    void reset() noexcept {
        entries_.clear();
        bytes_=0;
        sourceEpoch_.reset();
        ++stats_.resets;
    }
    // Caller supplies packet.pts from the actual *complete* compressed AU.
    // Null/invalid PTS cannot be joined safely to a decoded video image.
    void remember(std::int64_t pts,
                  std::shared_ptr<const FrameGtParts> gt,
                  std::size_t packetGtBytes,
                  Clock::time_point now=Clock::now()) {
        prune(now);
        if (pts==std::numeric_limits<std::int64_t>::min() || !gt) {
            ++stats_.missingPts; return;
        }
        if (gt->source) {
            const auto epoch=gt->source->key.streamEpoch;
            if (sourceEpoch_ && *sourceEpoch_!=epoch) reset();
            sourceEpoch_=epoch;
        }
        if (packetGtBytes>kMaxPendingBytes) {++stats_.sizeEvicted;return;}
        if (auto it=entries_.find(pts);it!=entries_.end()) {
            // A PTS collision cannot be resolved by packet arrival order.
            ++stats_.duplicatePts;
            if ((!it->second.gt->source && !gt->source ||
                 it->second.gt->source && gt->source &&
                 it->second.gt->source->key==gt->source->key &&
                 it->second.gt->source->sourceTimeNs==gt->source->sourceTimeNs) &&
                !it->second.conflict &&
                it->second.bytes==packetGtBytes) return;
            it->second.conflict=true;
            ++stats_.conflictingPts;
            return;
        }
        entries_.emplace(pts, Entry{std::move(gt),now,packetGtBytes,false});
        bytes_+=packetGtBytes;
        ++stats_.inserted;
        prune(now);
    }
    // Exact frame.pts / best_effort_timestamp only. No DTS/FIFO fallback.
    [[nodiscard]] std::shared_ptr<const FrameGtParts> take(
        std::int64_t presentationPts,
        Clock::time_point now=Clock::now()) {
        prune(now);
        if (presentationPts==std::numeric_limits<std::int64_t>::min()) {
            ++stats_.missingPts;return {};
        }
        const auto it=entries_.find(presentationPts);
        if (it==entries_.end()) {++stats_.misses;return {};}
        auto found=it->second.gt;
        const bool conflict=it->second.conflict;
        bytes_-=it->second.bytes;
        entries_.erase(it);
        if (conflict) {++stats_.conflictingPts;return {};}
        ++stats_.matched;
        return found;
    }
    [[nodiscard]] Stats stats() const noexcept {return stats_;}
    [[nodiscard]] std::size_t pendingBytes() const noexcept {return bytes_;}
    [[nodiscard]] std::size_t pendingFrames() const noexcept {return entries_.size();}

private:
    struct Entry {
        std::shared_ptr<const FrameGtParts> gt;
        Clock::time_point inserted;
        std::size_t bytes{};
        bool conflict{};
    };
    void prune(Clock::time_point now) {
        // Age eviction is deterministic even if video PTS order changes.
        for (auto it=entries_.begin(); it!=entries_.end();) {
            if (now>=it->second.inserted &&
                now-it->second.inserted>std::chrono::seconds(2)) {
                bytes_-=it->second.bytes;
                it=entries_.erase(it);
                ++stats_.ageEvicted;
            } else ++it;
        }
        while ((entries_.size()>128U || bytes_>kMaxPendingBytes) &&
               !entries_.empty()) {
            auto oldest=entries_.begin();
            for (auto it=entries_.begin();it!=entries_.end();++it)
                if (it->second.inserted<oldest->second.inserted) oldest=it;
            bytes_-=oldest->second.bytes;
            entries_.erase(oldest);
            ++stats_.sizeEvicted;
        }
    }
    static constexpr std::size_t kMaxPendingBytes=32U*1024U*1024U;
    std::map<std::int64_t,Entry> entries_;
    std::size_t bytes_{};
    std::optional<std::uint64_t> sourceEpoch_;
    Stats stats_{};
};

} // namespace reg::media
