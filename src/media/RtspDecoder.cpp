#include "media/RtspDecoder.hpp"

#include "media/FfmpegError.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <cerrno>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace reg::media {
namespace {

AVPixelFormat selectVulkanFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
        if (*current == AV_PIX_FMT_VULKAN) {
            return *current;
        }
    }

    // Software fallback is an architecture violation. Returning NONE forces failure.
    return AV_PIX_FMT_NONE;
}

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

} // namespace

RtspDecoder::RtspDecoder(AVBufferRef* vulkanDevice, RtspDecoderConfig config)
    : config_(std::move(config)) {
    if (vulkanDevice == nullptr) {
        throw std::invalid_argument("RtspDecoder requires a Vulkan AVHWDeviceContext");
    }

    vulkanDevice_ = av_buffer_ref(vulkanDevice);
    if (vulkanDevice_ == nullptr) {
        throw std::bad_alloc{};
    }

    checkFfmpeg(avformat_network_init(), "avformat_network_init");
}

RtspDecoder::~RtspDecoder() {
    requestStop();
    close();
    av_buffer_unref(&vulkanDevice_);
    avformat_network_deinit();
}

void RtspDecoder::run(const FrameCallback& onFrame) {
    openInput();
    openDecoder();

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        throw std::bad_alloc{};
    }

    std::uint64_t packetCount = 0;
    while (!stopRequested_.load(std::memory_order_acquire)) {
        const int result = av_read_frame(formatContext_, packet.get());
        if (result == AVERROR_EXIT && stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (result == AVERROR(EAGAIN)) {
            continue;
        }
        if (result < 0) {
            throwFfmpegError("av_read_frame", result);
        }

        ++packetCount;
        if (packet->stream_index == videoStreamIndex_) {
            decodePacket(onFrame, packet.get(), frame.get());
        }
        av_packet_unref(packet.get());
    }

    std::cout << "[rtsp] stopped after " << packetCount << " demuxed packets\n";
}

void RtspDecoder::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

void RtspDecoder::openInput() {
    close();

    formatContext_ = avformat_alloc_context();
    if (formatContext_ == nullptr) {
        throw std::bad_alloc{};
    }
    formatContext_->interrupt_callback.callback = &RtspDecoder::interruptCallback;
    formatContext_->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "udp", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set_int(&options, "max_delay", config_.maxDelayUs, 0);
    av_dict_set_int(&options, "reorder_queue_size", config_.reorderQueueSize, 0);

    const int openResult = avformat_open_input(&formatContext_, config_.url.c_str(), nullptr, &options);

    if (options != nullptr) {
        const AVDictionaryEntry* entry = nullptr;
        while ((entry = av_dict_iterate(options, entry)) != nullptr) {
            std::cerr << "[rtsp] unused FFmpeg option: " << entry->key << '=' << entry->value << '\n';
        }
    }
    av_dict_free(&options);

    checkFfmpeg(openResult, "avformat_open_input");
    checkFfmpeg(avformat_find_stream_info(formatContext_, nullptr), "avformat_find_stream_info");

    videoStreamIndex_ = av_find_best_stream(
        formatContext_,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0);
    checkFfmpeg(videoStreamIndex_, "av_find_best_stream(video)");

    const AVCodecParameters* parameters = formatContext_->streams[videoStreamIndex_]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_H264) {
        throw std::runtime_error("The MVP accepts H.264 only");
    }

    std::cout << "[rtsp] connected: " << config_.url << '\n';
    std::cout << "[rtsp] coded size: " << parameters->width << 'x' << parameters->height << '\n';
}

void RtspDecoder::openDecoder() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        throw std::runtime_error("FFmpeg H.264 decoder is unavailable");
    }

    bool vulkanHwConfigFound = false;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* hwConfig = avcodec_get_hw_config(codec, index);
        if (hwConfig == nullptr) {
            break;
        }
        if (hwConfig->pix_fmt == AV_PIX_FMT_VULKAN &&
            (hwConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            vulkanHwConfigFound = true;
            break;
        }
    }
    if (!vulkanHwConfigFound) {
        throw std::runtime_error(
            "The selected FFmpeg H.264 decoder does not expose AV_PIX_FMT_VULKAN via a HW device context");
    }

    codecContext_ = avcodec_alloc_context3(codec);
    if (codecContext_ == nullptr) {
        throw std::bad_alloc{};
    }

    const AVCodecParameters* parameters = formatContext_->streams[videoStreamIndex_]->codecpar;
    checkFfmpeg(avcodec_parameters_to_context(codecContext_, parameters), "avcodec_parameters_to_context");

    codecContext_->hw_device_ctx = av_buffer_ref(vulkanDevice_);
    if (codecContext_->hw_device_ctx == nullptr) {
        throw std::bad_alloc{};
    }

    codecContext_->get_format = &selectVulkanFormat;
    codecContext_->extra_hw_frames = config_.extraHwFrames;
    codecContext_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    checkFfmpeg(avcodec_open2(codecContext_, codec, nullptr), "avcodec_open2(H.264 Vulkan)");

    std::cout << "[decoder] opened H.264 decoder with extra_hw_frames="
              << config_.extraHwFrames << '\n';
}

void RtspDecoder::close() noexcept {
    if (codecContext_ != nullptr) {
        avcodec_free_context(&codecContext_);
    }
    if (formatContext_ != nullptr) {
        avformat_close_input(&formatContext_);
    }
    videoStreamIndex_ = -1;
}

void RtspDecoder::decodePacket(
    const FrameCallback& onFrame,
    AVPacket* packet,
    AVFrame* frame) {
    const auto drainFrames = [&]() {
        while (true) {
            const int receiveResult = avcodec_receive_frame(codecContext_, frame);
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return;
            }
            checkFfmpeg(receiveResult, "avcodec_receive_frame");

            if (frame->format != AV_PIX_FMT_VULKAN) {
                throw std::runtime_error(
                    "Decoder returned a non-Vulkan frame. CPU/software decode fallback is forbidden.");
            }

            onFrame(video::VideoFrame::cloneFrom(frame));
            av_frame_unref(frame);
        }
    };

    int sendResult = avcodec_send_packet(codecContext_, packet);
    if (sendResult == AVERROR(EAGAIN)) {
        drainFrames();
        sendResult = avcodec_send_packet(codecContext_, packet);
    }
    checkFfmpeg(sendResult, "avcodec_send_packet");
    drainFrames();
}

int RtspDecoder::interruptCallback(void* opaque) {
    const auto* self = static_cast<const RtspDecoder*>(opaque);
    return self->stopRequested_.load(std::memory_order_acquire) ? 1 : 0;
}

} // namespace reg::media
