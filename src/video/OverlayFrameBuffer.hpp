#pragma once

#include "media/FrameIdentity.hpp"
#include "video/VideoFrame.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace reg::video {

struct BufferedOverlayFrame {
    media::FrameKey key{};
    VideoFramePtr video{};
    std::chrono::steady_clock::time_point decodedAt{};
    std::chrono::steady_clock::time_point deadline{};
};

enum class OverlayPushResult {
    Inserted,
    MissingIdentity,
    EvictedOldest,
};

class OverlayFrameBuffer final {
public:
    explicit OverlayFrameBuffer(
        std::chrono::milliseconds playoutDelay = std::chrono::milliseconds{150},
        std::size_t capacity = 64);

    OverlayPushResult push(VideoFramePtr frame);

    // Replay/tests may already have a canonical FrameKey independently of an
    // in-memory VideoFrame. This overload intentionally supports that path.
    OverlayPushResult push(
        media::FrameKey key,
        VideoFramePtr frame,
        std::chrono::steady_clock::time_point decodedAt);

    std::optional<BufferedOverlayFrame> takeDue(
        std::chrono::steady_clock::time_point now);

    std::optional<BufferedOverlayFrame> front() const;

    void setPlayoutDelay(std::chrono::milliseconds delay);
    std::chrono::milliseconds playoutDelay() const;

    void clear();
    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::chrono::milliseconds playoutDelay_;
    std::size_t capacity_{};
    std::deque<BufferedOverlayFrame> frames_;
};

} // namespace reg::video
