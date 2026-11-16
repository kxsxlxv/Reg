#pragma once

#include "media/GtSei.hpp"
#include "media/GtAccessUnit.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace reg::diagnostics {

// Local per-process KGT1 diagnostics. One observe() per decoded video frame;
// no locks, JSON copies, file writes or labels carrying per-frame identity.
struct GtIngestSnapshot {
    std::uint64_t framesWithKgt1{};
    std::uint64_t gtReady{};
    std::uint64_t gtParseErrors{};
    std::uint64_t gtTooLarge{};
    std::uint64_t imuReady{};
    std::uint64_t imuInvalid{};
    std::uint64_t imuNull{};
    std::uint64_t imuSamplesParsed{};
    std::uint64_t gtParseTotalUs{}; // successful KGT1 parse only
    std::uint64_t gtParseMaxUs{};   // successful KGT1 parse only
    std::uint64_t gtJsonMaxBytes{};
    std::uint64_t skyMissing{},skyEncoded{},skyNonReady{},skyInvalid{};
    std::uint64_t depthMissing{},depthEncoded{},depthNonReady{},depthInvalid{};
    std::uint64_t auMalformed{},auConflicts{},auIdenticalDuplicates{};
};

class GtIngestStats final {
public:
    void observe(const media::GtMetadata& gt) noexcept {
        if (gt.status == media::GtStatus::Missing) return;
        framesWithKgt1_.fetch_add(1U, std::memory_order_relaxed);
        if (gt.status == media::GtStatus::Ready)
            gtReady_.fetch_add(1U, std::memory_order_relaxed);
        if (gt.status == media::GtStatus::ParseError)
            gtParseErrors_.fetch_add(1U, std::memory_order_relaxed);
        if (gt.status == media::GtStatus::ParseError &&
            gt.jsonBytes > media::kMaxGtJsonBytes)
            gtTooLarge_.fetch_add(1U, std::memory_order_relaxed);
        if (gt.cameraImu.status == media::CameraImuStatus::Ready) {
            imuReady_.fetch_add(1U, std::memory_order_relaxed);
            imuSamplesParsed_.fetch_add(
                static_cast<std::uint64_t>(gt.cameraImu.imu.size()),
                std::memory_order_relaxed);
        } else if (gt.cameraImu.status == media::CameraImuStatus::Invalid) {
            imuInvalid_.fetch_add(1U, std::memory_order_relaxed);
        } else if (gt.cameraImu.status == media::CameraImuStatus::Null) {
            imuNull_.fetch_add(1U, std::memory_order_relaxed);
        }
        if (gt.status == media::GtStatus::Ready) {
            gtParseTotalUs_.fetch_add(gt.parseDurationUs, std::memory_order_relaxed);
            maxStore(gtParseMaxUs_, gt.parseDurationUs);
        }
        maxStore(gtJsonMaxBytes_, static_cast<std::uint64_t>(gt.jsonBytes));
    }

    void observeMaps(const media::FrameGtParts& parts) noexcept {
        auto count=[](media::GtMapStatus status,
                      std::atomic_uint64_t& missing,
                      std::atomic_uint64_t& encoded,
                      std::atomic_uint64_t& nonReady,
                      std::atomic_uint64_t& invalid) {
            switch(status) {
            case media::GtMapStatus::Missing:
                ++missing; break;
            case media::GtMapStatus::Encoded:
            case media::GtMapStatus::Ready:
                ++encoded; break;
            case media::GtMapStatus::NotReady:
            case media::GtMapStatus::AmbiguousViewport:
            case media::GtMapStatus::Unavailable:
                ++nonReady; break;
            case media::GtMapStatus::Invalid:
            case media::GtMapStatus::FrameMismatch:
                ++invalid; break;
            }
        };
        count(parts.sky.status,skyMissing_,skyEncoded_,skyNonReady_,skyInvalid_);
        count(parts.depth.status,depthMissing_,depthEncoded_,depthNonReady_,depthInvalid_);
        if(parts.malformedAu || parts.oversizedAu)
            auMalformed_.fetch_add(1U,std::memory_order_relaxed);
        auConflicts_.fetch_add(parts.conflictingFields,std::memory_order_relaxed);
        auIdenticalDuplicates_.fetch_add(parts.identicalDuplicates,
                                         std::memory_order_relaxed);
    }

    GtIngestSnapshot snapshot() const noexcept {
        return {
            framesWithKgt1_.load(std::memory_order_relaxed),
            gtReady_.load(std::memory_order_relaxed),
            gtParseErrors_.load(std::memory_order_relaxed),
            gtTooLarge_.load(std::memory_order_relaxed),
            imuReady_.load(std::memory_order_relaxed),
            imuInvalid_.load(std::memory_order_relaxed),
            imuNull_.load(std::memory_order_relaxed),
            imuSamplesParsed_.load(std::memory_order_relaxed),
            gtParseTotalUs_.load(std::memory_order_relaxed),
            gtParseMaxUs_.load(std::memory_order_relaxed),
            gtJsonMaxBytes_.load(std::memory_order_relaxed),
            skyMissing_.load(std::memory_order_relaxed),
            skyEncoded_.load(std::memory_order_relaxed),
            skyNonReady_.load(std::memory_order_relaxed),
            skyInvalid_.load(std::memory_order_relaxed),
            depthMissing_.load(std::memory_order_relaxed),
            depthEncoded_.load(std::memory_order_relaxed),
            depthNonReady_.load(std::memory_order_relaxed),
            depthInvalid_.load(std::memory_order_relaxed),
            auMalformed_.load(std::memory_order_relaxed),
            auConflicts_.load(std::memory_order_relaxed),
            auIdenticalDuplicates_.load(std::memory_order_relaxed),
        };
    }
private:
    static void maxStore(std::atomic_uint64_t& slot, std::uint64_t value) noexcept {
        auto previous = slot.load(std::memory_order_relaxed);
        while (value > previous && !slot.compare_exchange_weak(
            previous, value, std::memory_order_relaxed)) {}
    }
    std::atomic_uint64_t framesWithKgt1_{0};
    std::atomic_uint64_t gtReady_{0};
    std::atomic_uint64_t gtParseErrors_{0};
    std::atomic_uint64_t gtTooLarge_{0};
    std::atomic_uint64_t imuReady_{0};
    std::atomic_uint64_t imuInvalid_{0};
    std::atomic_uint64_t imuNull_{0};
    std::atomic_uint64_t imuSamplesParsed_{0};
    std::atomic_uint64_t gtParseTotalUs_{0};
    std::atomic_uint64_t gtParseMaxUs_{0};
    std::atomic_uint64_t gtJsonMaxBytes_{0};
    std::atomic_uint64_t skyMissing_{0},skyEncoded_{0},skyNonReady_{0},skyInvalid_{0};
    std::atomic_uint64_t depthMissing_{0},depthEncoded_{0},depthNonReady_{0},depthInvalid_{0};
    std::atomic_uint64_t auMalformed_{0},auConflicts_{0},auIdenticalDuplicates_{0};
};

} // namespace reg::diagnostics
