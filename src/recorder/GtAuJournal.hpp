#pragma once

#include "media/CompressedVideoPacket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace reg::recorder {

struct GtAuJournalConfig {
    std::filesystem::path path{"blackbox/benchmark.rgau"};
    std::size_t maxQueueBytes{64U*1024U*1024U};
    std::size_t maxQueuePackets{1024U};
    std::uint64_t maxFileBytes{8ULL*1024ULL*1024ULL*1024ULL};
};

struct GtAuJournalStats {
    std::uint64_t accepted{},written{},dropped{},fileErrors{},bytesWritten{};
    std::size_t queuedPackets{},queuedBytes{};
    bool failed{},stopped{};
};

// Benchmark-only unmodified H264 AU journal. The input is the same ref-counted
// AVPacket delivered to the recorder, *before* any leaky Raw display mailbox.
// Worker writes original compressed bytes and exact packet PTS/DTS/timebase.
// No video AVFrame, graphics resource or disk IO in submit().
class GtAuJournal final {
public:
    explicit GtAuJournal(GtAuJournalConfig config={});
    ~GtAuJournal();
    GtAuJournal(const GtAuJournal&)=delete;
    GtAuJournal& operator=(const GtAuJournal&)=delete;

    bool submit(media::CompressedVideoPacketPtr packet,
                std::uint64_t decoderSession) noexcept;
    GtAuJournalStats stats() const noexcept;
    void stop() noexcept;
private:
    struct Entry {
        media::CompressedVideoPacketPtr packet;
        std::uint64_t decoderSession{};
        std::size_t bytes{};
    };
    void run(std::stop_token token) noexcept;
    void fail() noexcept;
    void manifest() noexcept;
    GtAuJournalConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Entry> pending_;
    std::size_t pendingBytes_{};
    std::jthread worker_;
    std::atomic_uint64_t accepted_{0},written_{0},dropped_{0},
                         fileErrors_{0},bytesWritten_{0};
    std::atomic_bool failed_{false},stopped_{false};
};

// Read-only streaming replay: verifies RGAU1 records, size bounds, and CRC
// before returning bytes. Never treats an incomplete/truncated capture as valid.
struct GtAuJournalRecord {
    std::uint64_t decoderSession{};
    std::int64_t pts{},dts{};
    std::int32_t timeBaseNum{},timeBaseDen{};
    std::vector<std::uint8_t> bytes;
};
class GtAuJournalReader final {
public:
    explicit GtAuJournalReader(const std::filesystem::path& path);
    std::optional<GtAuJournalRecord> next();
private:
    std::ifstream input_;
};

} // namespace reg::recorder
