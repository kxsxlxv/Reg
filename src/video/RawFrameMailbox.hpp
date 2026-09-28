#pragma once

#include "video/VideoFrame.hpp"

#include <atomic>
#include <memory>

namespace reg::video {

// Raw-display semantic: newest frame wins. This is intentionally not a FIFO.
class RawFrameMailbox final {
public:
    void publish(VideoFramePtr frame) noexcept {
        latest_.store(std::move(frame), std::memory_order_release);
    }

    VideoFramePtr latest() const noexcept {
        return latest_.load(std::memory_order_acquire);
    }

    void clear() noexcept {
        latest_.store({}, std::memory_order_release);
    }

private:
    std::atomic<VideoFramePtr> latest_{};
};

} // namespace reg::video
