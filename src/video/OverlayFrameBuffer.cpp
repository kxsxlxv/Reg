#include "video/OverlayFrameBuffer.hpp"

#include <stdexcept>
#include <utility>

namespace reg::video {

OverlayFrameBuffer::OverlayFrameBuffer(
    std::chrono::milliseconds playoutDelay,
    std::size_t capacity)
    : playoutDelay_(playoutDelay),
      capacity_(capacity) {
    if (playoutDelay_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Overlay playout delay must not be negative");
    }
    if (capacity_ == 0) {
        throw std::invalid_argument("OverlayFrameBuffer capacity must be greater than zero");
    }
}

OverlayPushResult OverlayFrameBuffer::push(VideoFramePtr frame) {
    if (!frame) {
        return OverlayPushResult::MissingIdentity;
    }

    const auto identity = frame->identity();
    if (!identity) {
        return OverlayPushResult::MissingIdentity;
    }

    return push(identity->key, std::move(frame), frame->decodedAt());
}

OverlayPushResult OverlayFrameBuffer::push(
    media::FrameKey key,
    VideoFramePtr frame,
    std::chrono::steady_clock::time_point decodedAt) {
    std::scoped_lock lock(mutex_);

    OverlayPushResult result = OverlayPushResult::Inserted;
    if (frames_.size() >= capacity_) {
        frames_.pop_front();
        result = OverlayPushResult::EvictedOldest;
    }

    frames_.push_back(BufferedOverlayFrame{
        .key = key,
        .video = std::move(frame),
        .decodedAt = decodedAt,
        .deadline = decodedAt + playoutDelay_,
    });

    return result;
}

std::optional<BufferedOverlayFrame> OverlayFrameBuffer::takeDue(
    std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);

    if (frames_.empty() || frames_.front().deadline > now) {
        return std::nullopt;
    }

    BufferedOverlayFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

std::optional<BufferedOverlayFrame> OverlayFrameBuffer::front() const {
    std::scoped_lock lock(mutex_);
    if (frames_.empty()) {
        return std::nullopt;
    }
    return frames_.front();
}

void OverlayFrameBuffer::setPlayoutDelay(std::chrono::milliseconds delay) {
    if (delay < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Overlay playout delay must not be negative");
    }

    std::scoped_lock lock(mutex_);

    // Existing buffered frames keep their relative decoded time but receive a
    // new deadline. This is deterministic and suitable for a future operator
    // adjustment before adaptive delay is introduced.
    playoutDelay_ = delay;
    for (auto& frame : frames_) {
        frame.deadline = frame.decodedAt + playoutDelay_;
    }
}

std::chrono::milliseconds OverlayFrameBuffer::playoutDelay() const {
    std::scoped_lock lock(mutex_);
    return playoutDelay_;
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
