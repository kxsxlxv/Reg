#pragma once

#include "metadata/FrameMetadata.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace reg::recorder {

struct MetadataJournalConfig {
    std::filesystem::path directory{"blackbox"};
    std::chrono::milliseconds targetSegmentDuration{
        std::chrono::seconds{5}};
    std::chrono::milliseconds retention{
        std::chrono::minutes{5}};
};

struct MetadataJournalStats {
    std::uint64_t recordsWritten{};
    std::uint64_t segmentsCompleted{};
    std::uint64_t segmentsDeleted{};
};

struct MetadataJournalRecord {
    std::chrono::nanoseconds receiveOffset{};
    metadata::FrameMetadata metadata{};
};

struct MetadataJournalFile {
    std::uint64_t streamEpoch{};
    std::uint64_t segmentStartUnixNs{};
    std::vector<MetadataJournalRecord> records;
};

class MetadataJournalWriter final {
public:
    explicit MetadataJournalWriter(
        MetadataJournalConfig config = {});
    ~MetadataJournalWriter();

    MetadataJournalWriter(
        const MetadataJournalWriter&) = delete;
    MetadataJournalWriter& operator=(
        const MetadataJournalWriter&) = delete;

    void write(const metadata::FrameMetadata& metadata);
    void close();

    const MetadataJournalStats& stats() const noexcept {
        return stats_;
    }

private:
    struct CompletedSegment {
        std::filesystem::path path;
        std::chrono::milliseconds duration{};
    };

    void openSegment(
        const metadata::FrameMetadata& metadata,
        std::chrono::steady_clock::time_point receivedAt);
    void closeSegment();
    void enforceRetention();
    std::filesystem::path nextSegmentPath(
        std::uint64_t streamEpoch);

    MetadataJournalConfig config_;
    std::ofstream stream_;

    std::filesystem::path currentPath_;
    std::uint64_t currentEpoch_{};
    std::uint64_t segmentStartUnixNs_{};
    std::chrono::steady_clock::time_point segmentOpenedAt_{};
    std::chrono::steady_clock::time_point lastRecordAt_{};

    std::deque<CompletedSegment> completed_;
    std::chrono::milliseconds completedDuration_{};

    std::uint64_t nextSegmentIndex_{0};
    MetadataJournalStats stats_{};
};

MetadataJournalFile readMetadataJournalFile(
    const std::filesystem::path& path);

} // namespace reg::recorder
