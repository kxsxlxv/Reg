#pragma once

#include "media/CompressedVideoPacket.hpp"
#include "media/VideoStreamDescriptor.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>

struct AVFormatContext;
struct AVStream;

namespace reg::recorder {

struct SegmentWriterConfig {
    std::filesystem::path directory{"blackbox"};
    std::chrono::milliseconds targetSegmentDuration{
        std::chrono::seconds{5}};
    std::chrono::milliseconds retention{
        std::chrono::minutes{5}};
};

enum class SegmentWriteResult {
    Written,
    WaitingForKeyframe,
    Rotated,
};

struct SegmentWriterStats {
    std::uint64_t packetsWritten{};
    std::uint64_t packetsWaitingForKeyframe{};
    std::uint64_t segmentsCompleted{};
    std::uint64_t segmentsDeleted{};
};

class SegmentedMkvWriter final {
public:
    explicit SegmentedMkvWriter(
        SegmentWriterConfig config = {});
    ~SegmentedMkvWriter();

    SegmentedMkvWriter(
        const SegmentedMkvWriter&) = delete;
    SegmentedMkvWriter& operator=(
        const SegmentedMkvWriter&) = delete;

    void setStreamDescriptor(
        media::VideoStreamDescriptorPtr descriptor);

    SegmentWriteResult write(
        const media::CompressedVideoPacket& packet);

    void close();

    const SegmentWriterStats& stats() const noexcept {
        return stats_;
    }

    const std::filesystem::path&
    currentPath() const noexcept {
        return currentPath_;
    }

private:
    struct CompletedSegment {
        std::filesystem::path path;
        std::chrono::milliseconds duration{};
    };

    void openSegment(
        const media::CompressedVideoPacket& firstPacket);

    void closeSegment();
    void enforceRetention();

    std::filesystem::path nextSegmentPath();

    static std::int64_t packetReferenceTimestamp(
        const AVPacket* packet) noexcept;

    SegmentWriterConfig config_;
    media::VideoStreamDescriptorPtr descriptor_;

    AVFormatContext* output_{nullptr};
    AVStream* outputStream_{nullptr};

    std::filesystem::path currentPath_;
    std::chrono::steady_clock::time_point
        segmentOpenedAt_{};
    std::int64_t segmentBaseTimestamp_{
        AV_NOPTS_VALUE};

    std::deque<CompletedSegment> completed_;
    std::chrono::milliseconds completedDuration_{};

    std::uint64_t nextSegmentIndex_{0};
    SegmentWriterStats stats_{};
};

} // namespace reg::recorder
