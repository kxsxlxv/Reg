#pragma once

#include <memory>

extern "C" {
#include <libavutil/rational.h>
}

struct AVCodecParameters;

namespace reg::media {

class VideoStreamDescriptor final {
public:
    static std::shared_ptr<
        const VideoStreamDescriptor>
    cloneFrom(
        const AVCodecParameters* parameters,
        AVRational timeBase);

    ~VideoStreamDescriptor();

    VideoStreamDescriptor(
        const VideoStreamDescriptor&) = delete;
    VideoStreamDescriptor& operator=(
        const VideoStreamDescriptor&) = delete;

    const AVCodecParameters* parameters() const noexcept {
        return parameters_;
    }

    AVRational timeBase() const noexcept {
        return timeBase_;
    }

private:
    VideoStreamDescriptor(
        AVCodecParameters* parameters,
        AVRational timeBase) noexcept;

    AVCodecParameters* parameters_{nullptr};
    AVRational timeBase_{};
};

using VideoStreamDescriptorPtr =
    std::shared_ptr<const VideoStreamDescriptor>;

} // namespace reg::media
