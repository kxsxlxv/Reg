#pragma once

#include "diagnostics/ExactSyncDiagnostics.hpp"
#include "metadata/FrameMetadata.hpp"
#include "telemetry/GtRangeHistory.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace reg::telemetry {

enum class Severity {
    Info,
    Warning,
    Error,
};

struct Event {
    std::chrono::steady_clock::time_point time{};
    Severity severity{Severity::Info};
    std::string message;
};

struct Counters {
    bool rtspConnected{};
    bool videoSignalPresent{};
    std::uint64_t videoFrameAgeMs{};
    bool cvSignalPresent{};
    std::uint64_t cvPacketAgeMs{};
    std::uint64_t decoderSessions{};
    std::uint64_t reconnects{};

    // Per-process counters, published independently of the Telemetry window.
    std::uint64_t demuxedVideoPackets{}; // compressed packets, never FPS
    std::uint64_t gtKgt1Frames{};
    std::uint64_t gtReadyFrames{};
    std::uint64_t gtParseErrors{};
    std::uint64_t gtOversizePackets{};
    std::uint64_t imuReadyFrames{};
    std::uint64_t imuInvalidFrames{};
    std::uint64_t imuNullFrames{};
    std::uint64_t imuSamplesParsed{}; // overlapping per-frame history
    std::uint64_t gtParseTotalUs{}; // successful GT only
    std::uint64_t gtParseMaxUs{};
    std::uint64_t gtMaxJsonBytes{};
    std::uint64_t skyMissingFrames{},skyEncodedFrames{},skyNonReadyFrames{},skyInvalidFrames{};
    std::uint64_t depthMissingFrames{},depthEncodedFrames{},depthNonReadyFrames{},depthInvalidFrames{};
    std::uint64_t gtMalformedAu{},gtConflictingSei{},gtIdenticalDuplicates{};
    std::uint64_t gtWorkerQueueDrops{},gtWorkerLockDrops{};
    std::uint64_t gtWorkerSkyReady{},gtWorkerDepthReady{};
    std::uint64_t gtWorkerInvalidSky{},gtWorkerInvalidDepth{};
    std::uint64_t gtWorkerChecksumFailures{};
    std::uint64_t gtWorkerTotalUs{},gtWorkerMaxUs{};
    std::size_t gtWorkerQueueBytes{},gtWorkerQueueFrames{};
    std::uint64_t gtWorkerQueueAgeMs{};
    std::size_t gtWorkerCacheFrames{},gtWorkerCacheBytes{};
    std::uint64_t gtWorkerEvictedAge{},gtWorkerEvictedCapacity{};
    std::uint64_t gtWorkerLookupHits{},gtWorkerLookupMisses{},gtWorkerLookupBusy{};
    bool gtBenchmarkEnabled{};
    bool gtBenchmarkFailed{};
    std::uint64_t gtArchiveAccepted{},gtArchiveWritten{},gtArchiveDropped{},
                  gtArchiveFileErrors{},gtArchiveBytesWritten{};
    std::size_t gtArchivePendingBytes{},gtArchivePendingPackets{};
    std::uint64_t decodedFrames{};
    std::uint64_t rawPresentedFrames{};
    std::uint64_t overlayPresentedFrames{};
    std::uint64_t overlayMissingMetadataDrops{};
    std::uint64_t overlayBufferEvictions{};
    std::uint64_t overlayMissingIdentity{};

    std::uint64_t identityMissingFrames{};
    std::uint64_t metadataPackets{};
    std::uint64_t metadataValidFrames{};
    std::uint64_t metadataAcceptedFrames{};
    std::uint64_t metadataTargets{};
    std::uint64_t metadataInvalid{};
    std::uint64_t metadataDuplicates{};
    std::uint64_t metadataSequenceGaps{};
    std::uint64_t metadataOutOfOrder{};
    std::uint64_t metadataStoreEvictions{};

    diagnostics::ExactSyncSnapshot exactSync{};
    std::uint32_t configuredOverlayDelayMs{};

    std::uint64_t recorderPacketsWritten{};
    std::uint64_t recorderQueueDrops{};
    std::uint64_t recorderMetadataWritten{};
    std::uint64_t recorderMetadataQueueDrops{};
    std::uint64_t recorderFailures{};

    std::size_t overlayBufferDepth{};
    std::size_t metadataStoreDepth{};
    std::size_t recorderQueueDepth{};
};

struct Sample {
    std::chrono::steady_clock::time_point time{};
    Counters counters{};
};

struct Target {
    std::uint64_t id{};
    std::uint16_t classId{};
    std::uint16_t flags{};
    float confidence{};
    metadata::NormalizedRect bbox{};
};

struct Snapshot {
    Counters counters{};
    std::vector<Sample> samples;
    std::vector<Event> events;
    std::vector<Target> targets;
    std::shared_ptr<const std::vector<GtRangeTrack>> gtRangeTracks;
    GtCvDiagnostics gtCvDiagnostics{};
};

class TelemetryModel final {
public:
    explicit TelemetryModel(
        std::chrono::milliseconds retention =
            std::chrono::minutes{5},
        std::chrono::milliseconds sampleInterval =
            std::chrono::milliseconds{250},
        std::size_t maxEvents = 4096);

    void updateCounters(
        Counters counters,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    void setTargets(
        const metadata::FrameMetadata& metadata);

    void clearTargets();

    void observeGtRange(const media::GtMetadata& gt,
                        std::chrono::steady_clock::time_point when);
    void observeCvForGt(const metadata::FrameMetadata& metadata);
    void resetGtRanges();

    void log(
        Severity severity,
        std::string message,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    Snapshot snapshot() const;

private:
    void pruneLocked(
        std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex_;

    std::chrono::milliseconds retention_;
    std::chrono::milliseconds sampleInterval_;
    std::size_t maxEvents_{};

    Counters counters_{};
    std::deque<Sample> samples_;
    std::deque<Event> events_;
    std::vector<Target> targets_;
    // Separate lock: render snapshots must not block video/CVM1 threads.
    mutable std::mutex gtRangeMutex_;
    GtRangeHistory gtRanges_;
    // Reuse a 10 Hz immutable snapshot across faster UI frames.
    mutable std::shared_ptr<const std::vector<GtRangeTrack>> gtRangeSnapshot_;
    mutable std::chrono::steady_clock::time_point gtRangeSnapshotAt_{};

    std::chrono::steady_clock::time_point lastSampleTime_{};
};

const char* toString(Severity severity) noexcept;

} // namespace reg::telemetry
