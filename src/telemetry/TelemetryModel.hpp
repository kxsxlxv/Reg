#pragma once

#include "metadata/FrameMetadata.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
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
    std::uint64_t decoderSessions{};
    std::uint64_t reconnects{};

    std::uint64_t decodedFrames{};
    std::uint64_t rawPresentedFrames{};
    std::uint64_t overlayPresentedFrames{};
    std::uint64_t overlayMissingMetadataDrops{};

    std::uint64_t metadataPackets{};
    std::uint64_t metadataInvalid{};
    std::uint64_t metadataDuplicates{};
    std::uint64_t metadataSequenceGaps{};

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

    std::chrono::steady_clock::time_point lastSampleTime_{};
};

const char* toString(Severity severity) noexcept;

} // namespace reg::telemetry
