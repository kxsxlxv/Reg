#pragma once

#include "media/CompressedVideoPacket.hpp"
#include "media/VideoStreamDescriptor.hpp"
#include "video/VideoFrame.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace reg::media {

enum class VideoInputMode {
    RtspUdp,
    File,
};

struct RtspDecoderConfig {
    std::string url;
    VideoInputMode inputMode{VideoInputMode::RtspUdp};
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
};

struct RtspDecoderCallbacks {
    std::function<void(
        VideoStreamDescriptorPtr)> onStreamOpened;

    std::function<void(
        CompressedVideoPacketPtr)> onCompressedPacket;

    std::function<void(
        video::VideoFramePtr)> onFrame;
};

class RtspDecoder final {
public:
    using FrameCallback =
        std::function<void(video::VideoFramePtr)>;

    RtspDecoder(
        AVBufferRef* vulkanDevice,
        RtspDecoderConfig config);
    ~RtspDecoder();

    RtspDecoder(const RtspDecoder&) = delete;
    RtspDecoder& operator=(
        const RtspDecoder&) = delete;

    void run(const RtspDecoderCallbacks& callbacks);

    // Compatibility convenience for decode-only consumers.
    void run(const FrameCallback& onFrame);

    void requestStop() noexcept;

private:
    VideoStreamDescriptorPtr openInput();
    void openDecoder();
    void close() noexcept;

    void decodePacket(
        const FrameCallback& onFrame,
        AVPacket* packet,
        AVFrame* frame);

    static int interruptCallback(void* opaque);

    AVBufferRef* vulkanDevice_{nullptr};
    RtspDecoderConfig config_;

    AVFormatContext* formatContext_{nullptr};
    AVCodecContext* codecContext_{nullptr};
    int videoStreamIndex_{-1};

    std::atomic_bool stopRequested_{false};
};

} // namespace reg::media
