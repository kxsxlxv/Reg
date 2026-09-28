#include "media/VideoStreamDescriptor.hpp"

extern "C" {
#include <libavcodec/codec_par.h>
}

#include <new>
#include <stdexcept>

namespace reg::media {

VideoStreamDescriptor::VideoStreamDescriptor(
    AVCodecParameters* parameters,
    AVRational timeBase) noexcept
    : parameters_(parameters),
      timeBase_(timeBase) {}

VideoStreamDescriptor::~VideoStreamDescriptor() {
    avcodec_parameters_free(&parameters_);
}

std::shared_ptr<const VideoStreamDescriptor>
VideoStreamDescriptor::cloneFrom(
    const AVCodecParameters* parameters,
    AVRational timeBase) {
    if (parameters == nullptr) {
        throw std::invalid_argument(
            "VideoStreamDescriptor requires codec parameters");
    }

    AVCodecParameters* clone =
        avcodec_parameters_alloc();
    if (clone == nullptr) {
        throw std::bad_alloc{};
    }

    const int result =
        avcodec_parameters_copy(
            clone,
            parameters);
    if (result < 0) {
        avcodec_parameters_free(&clone);
        throw std::runtime_error(
            "avcodec_parameters_copy failed");
    }

    return std::shared_ptr<
        const VideoStreamDescriptor>(
            new VideoStreamDescriptor(
                clone,
                timeBase));
}

} // namespace reg::media
