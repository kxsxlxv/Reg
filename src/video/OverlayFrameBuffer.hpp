#pragma once

#include "media/FrameIdentity.hpp"
#include "video/VideoFrame.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace reg::video {

enum class OverlayPushResult {
    Accepted,
    MissingIdentity,
    EvictedOldestAndAccepted,
};

struct BufferedOverlayFrame {
    media::FrameKey key{};
    VideoFramePtr video;
    std::chrono::steady_clock::time_point decodedAt{};
    std::chrono::steady_clock::time_point deadline{};
};

class OverlayFrameBuffer final {
public:
    using Clock = std::chrono::steady_clock;

    explicit OverlayFrameBuffer(
        std::chrono::milliseconds delay = std::chrono::milliseconds(150),
        std::size_t maxFrames = 32);

    OverlayPushResult push(VideoFramePtr frame);

    std::optional<BufferedOverlayFrame> popDue(Clock::time_point now);

    void setDelay(std::chrono::milliseconds delay);
    std::chrono::milliseconds delay() const;

    void clear() noexcept;
    std::size_t size() const;
    std::size_t maxFrames() const noexcept { return maxFrames_; }

private:
    std::chrono::milliseconds delay_{};
    std::size_t maxFrames_{};

    mutable std::mutex mutex_;
    std::deque<BufferedOverlayFrame> frames_;
};

} // namespace reg::video
