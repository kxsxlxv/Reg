#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace reg::media {
class CompressedVideoPacket;
}

namespace reg::video {
class VideoFrame;
}

namespace reg::diagnostics {

// Low-overhead asynchronous CSV trace for diagnosing source/RTSP/decode/present
// cadence. Producers only format/enqueue rows; filesystem I/O stays on a
// dedicated writer thread so the diagnostic itself does not intentionally
// block decode or presentation.
class FrameTimingLogger final {
public:
    FrameTimingLogger();
    ~FrameTimingLogger();

    FrameTimingLogger(const FrameTimingLogger&) = delete;
    FrameTimingLogger& operator=(const FrameTimingLogger&) = delete;

    bool enabled() const noexcept;
    const std::filesystem::path& path() const noexcept;

    void beginSession(int timeBaseNum, int timeBaseDen);

    void logPacket(const media::CompressedVideoPacket& packet);

    void logDecodedFrame(
        const video::VideoFrame& frame,
        std::uint64_t decodedTotal);

    void logPresent(
        const video::VideoFrame& frame,
        std::uint64_t decodedTotal,
        std::uint64_t presentedTotal,
        std::chrono::steady_clock::time_point renderBegin,
        std::chrono::steady_clock::time_point renderEnd,
        std::uint64_t notReadyAttempts,
        double renderAttemptTotalMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::diagnostics
