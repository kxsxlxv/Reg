#include "video/OverlayFrameBuffer.hpp"

#include <stdexcept>
#include <utility>

namespace reg::video {

OverlayFrameBuffer::OverlayFrameBuffer(
    std::chrono::milliseconds delay,
    std::size_t maxFrames)
    : delay_(delay),
      maxFrames_(maxFrames) {
    if (delay_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Overlay playout delay must be non-negative");
    }
    if (maxFrames_ == 0) {
        throw std::invalid_argument("Overlay frame buffer must have at least one slot");
    }
}

OverlayPushResult OverlayFrameBuffer::push(VideoFramePtr frame) {
    if (!frame) {
        throw std::invalid_argument("OverlayFrameBuffer::push received a null frame");
    }

    const auto identity = frame->identity();
    if (!identity) {
        return OverlayPushResult::MissingIdentity;
    }

    BufferedOverlayFrame buffered{
        .key = identity->key,
        .video = std::move(frame),
        .decodedAt = {},
        .deadline = {},
    };
    buffered.decodedAt = buffered.video->decodedAt();

    std::scoped_lock lock(mutex_);
    buffered.deadline = buffered.decodedAt + delay_;

    bool evicted = false;
    if (frames_.size() >= maxFrames_) {
        frames_.pop_front();
        evicted = true;
    }

    frames_.push_back(std::move(buffered));
    return evicted
        ? OverlayPushResult::EvictedOldestAndAccepted
        : OverlayPushResult::Accepted;
}

std::optional<BufferedOverlayFrame> OverlayFrameBuffer::popDue(
    Clock::time_point now) {
    std::scoped_lock lock(mutex_);

    if (frames_.empty() || frames_.front().deadline > now) {
        return std::nullopt;
    }

    BufferedOverlayFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

void OverlayFrameBuffer::setDelay(std::chrono::milliseconds delay) {
    if (delay < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Overlay playout delay must be non-negative");
    }

    std::scoped_lock lock(mutex_);
    delay_ = delay;

    for (auto& frame : frames_) {
        frame.deadline = frame.decodedAt + delay_;
    }
}

std::chrono::milliseconds OverlayFrameBuffer::delay() const {
    std::scoped_lock lock(mutex_);
    return delay_;
}

void OverlayFrameBuffer::clear() {
    std::scoped_lock lock(mutex_);
    frames_.clear();
}

std::size_t OverlayFrameBuffer::size() const {
    std::scoped_lock lock(mutex_);
    return frames_.size();
}

} // namespace reg::video
