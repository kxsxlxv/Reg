#pragma once

#include "media/CompressedVideoPacket.hpp"
#include "media/VideoStreamDescriptor.hpp"
#include "recorder/SegmentedMkvWriter.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace reg::recorder {

struct BlackboxRecorderConfig {
    SegmentWriterConfig writer{};
    std::size_t queueCapacity{2048};
};

struct BlackboxRecorderStats {
    std::uint64_t packetsAccepted{};
    std::uint64_t packetsDroppedQueueFull{};
    std::uint64_t packetsWritten{};
    std::uint64_t packetsWaitingForKeyframe{};
    std::uint64_t segmentRotations{};
    std::uint64_t failures{};
    std::size_t queueDepth{};
    bool failed{};
};

class BlackboxRecorder final {
public:
    explicit BlackboxRecorder(
        BlackboxRecorderConfig config = {});
    ~BlackboxRecorder();

    BlackboxRecorder(
        const BlackboxRecorder&) = delete;
    BlackboxRecorder& operator=(
        const BlackboxRecorder&) = delete;

    // A new descriptor represents a new logical compressed stream session.
    // Pending packets from the previous descriptor are discarded so sessions
    // are never muxed into the same segment accidentally.
    void configure(
        media::VideoStreamDescriptorPtr descriptor);

    // Never waits for disk I/O. Returns false when the bounded queue is full
    // or the recorder has entered a failed/stopping state.
    bool submit(
        media::CompressedVideoPacketPtr packet) noexcept;

    void stop() noexcept;

    BlackboxRecorderStats stats() const noexcept;
    std::string lastError() const;

private:
    using QueueItem = std::variant<
        media::VideoStreamDescriptorPtr,
        media::CompressedVideoPacketPtr>;

    void workerMain() noexcept;
    void setFailure(std::string message) noexcept;

    BlackboxRecorderConfig config_;
    SegmentedMkvWriter writer_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueueItem> queue_;

    bool stopping_{false};
    bool failed_{false};
    std::string lastError_;

    std::thread worker_;

    std::atomic_uint64_t packetsAccepted_{0};
    std::atomic_uint64_t packetsDroppedQueueFull_{0};
    std::atomic_uint64_t packetsWritten_{0};
    std::atomic_uint64_t packetsWaitingForKeyframe_{0};
    std::atomic_uint64_t segmentRotations_{0};
    std::atomic_uint64_t failures_{0};
};

} // namespace reg::recorder
