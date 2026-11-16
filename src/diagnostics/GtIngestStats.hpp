#pragma once

#include "media/GtSei.hpp"

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
};

} // namespace reg::diagnostics
