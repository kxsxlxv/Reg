#include "recorder/BlackboxRecorder.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace reg::recorder {

BlackboxRecorder::BlackboxRecorder(
    BlackboxRecorderConfig config)
    : config_(std::move(config)),
      writer_(config_.writer) {
    if (config_.queueCapacity == 0) {
        throw std::invalid_argument(
            "blackbox queue capacity must be greater than zero");
    }

    worker_ = std::thread(
        [this] {
            workerMain();
        });
}

BlackboxRecorder::~BlackboxRecorder() {
    stop();
}

void BlackboxRecorder::configure(
    media::VideoStreamDescriptorPtr descriptor) {
    if (!descriptor) {
        throw std::invalid_argument(
            "blackbox recorder requires stream descriptor");
    }

    {
        std::scoped_lock lock(mutex_);

        if (stopping_ || failed_) {
            return;
        }

        // Descriptor boundaries are session boundaries. Dropping queued old
        // packets is safer than mixing packets from two codec configurations.
        queue_.clear();
        queue_.emplace_back(
            std::move(descriptor));
    }

    condition_.notify_one();
}

bool BlackboxRecorder::submit(
    media::CompressedVideoPacketPtr packet) noexcept {
    if (!packet) {
        return false;
    }

    {
        std::scoped_lock lock(mutex_);

        if (stopping_ || failed_) {
            return false;
        }

        if (queue_.size() >=
            config_.queueCapacity) {
            packetsDroppedQueueFull_.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }

        queue_.emplace_back(
            std::move(packet));
    }

    packetsAccepted_.fetch_add(
        1,
        std::memory_order_relaxed);

    condition_.notify_one();
    return true;
}

void BlackboxRecorder::stop() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            // Another caller/destructor already requested shutdown.
        } else {
            stopping_ = true;
        }
    }

    condition_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

BlackboxRecorderStats
BlackboxRecorder::stats() const noexcept {
    std::size_t depth = 0;
    bool failed = false;

    {
        std::scoped_lock lock(mutex_);
        depth = queue_.size();
        failed = failed_;
    }

    return BlackboxRecorderStats{
        .packetsAccepted =
            packetsAccepted_.load(
                std::memory_order_relaxed),
        .packetsDroppedQueueFull =
            packetsDroppedQueueFull_.load(
                std::memory_order_relaxed),
        .packetsWritten =
            packetsWritten_.load(
                std::memory_order_relaxed),
        .packetsWaitingForKeyframe =
            packetsWaitingForKeyframe_.load(
                std::memory_order_relaxed),
        .segmentRotations =
            segmentRotations_.load(
                std::memory_order_relaxed),
        .failures =
            failures_.load(
                std::memory_order_relaxed),
        .queueDepth = depth,
        .failed = failed,
    };
}

std::string BlackboxRecorder::lastError() const {
    std::scoped_lock lock(mutex_);
    return lastError_;
}

void BlackboxRecorder::workerMain() noexcept {
    try {
        while (true) {
            QueueItem item;

            {
                std::unique_lock lock(mutex_);
                condition_.wait(
                    lock,
                    [this] {
                        return stopping_ ||
                               !queue_.empty();
                    });

                if (queue_.empty()) {
                    if (stopping_) {
                        break;
                    }
                    continue;
                }

                item =
                    std::move(queue_.front());
                queue_.pop_front();
            }

            if (const auto* descriptor =
                    std::get_if<
                        media::VideoStreamDescriptorPtr>(
                            &item)) {
                writer_.setStreamDescriptor(
                    *descriptor);
                continue;
            }

            const auto& packet =
                std::get<
                    media::CompressedVideoPacketPtr>(
                        item);

            if (!packet) {
                continue;
            }

            const SegmentWriteResult result =
                writer_.write(*packet);

            if (result ==
                SegmentWriteResult::WaitingForKeyframe) {
                packetsWaitingForKeyframe_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            } else {
                packetsWritten_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            if (result ==
                SegmentWriteResult::Rotated) {
                segmentRotations_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }

        writer_.close();
    } catch (const std::exception& error) {
        setFailure(error.what());
    } catch (...) {
        setFailure(
            "unknown blackbox recorder failure");
    }
}

void BlackboxRecorder::setFailure(
    std::string message) noexcept {
    failures_.fetch_add(
        1,
        std::memory_order_relaxed);

    {
        std::scoped_lock lock(mutex_);
        failed_ = true;
        lastError_ = std::move(message);
        queue_.clear();
        stopping_ = true;
    }

    condition_.notify_all();
}

} // namespace reg::recorder
