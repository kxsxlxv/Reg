#include "recorder/SegmentedMkvWriter.hpp"

#include "media/FfmpegError.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace reg::recorder {
namespace {

struct PacketGuard {
    AVPacket* packet{nullptr};

    ~PacketGuard() {
        av_packet_free(&packet);
    }
};

std::tm localTime(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

} // namespace

SegmentedMkvWriter::SegmentedMkvWriter(
    SegmentWriterConfig config)
    : config_(std::move(config)) {
    if (config_.targetSegmentDuration <=
        std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "segment duration must be positive");
    }

    if (config_.retention <
        config_.targetSegmentDuration) {
        throw std::invalid_argument(
            "retention must be at least one segment");
    }

    std::filesystem::create_directories(
        config_.directory);
}

SegmentedMkvWriter::~SegmentedMkvWriter() {
    try {
        close();
    } catch (...) {
        // Destructors must not propagate disk/muxer errors.
    }
}

void SegmentedMkvWriter::setStreamDescriptor(
    media::VideoStreamDescriptorPtr descriptor) {
    if (!descriptor ||
        descriptor->parameters() == nullptr) {
        throw std::invalid_argument(
            "SegmentedMkvWriter requires stream descriptor");
    }

    if (output_ != nullptr) {
        closeSegment();
    }

    descriptor_ = std::move(descriptor);
}

SegmentWriteResult SegmentedMkvWriter::write(
    const media::CompressedVideoPacket& packet) {
    if (!descriptor_) {
        throw std::logic_error(
            "stream descriptor must be set before packets");
    }

    const bool shouldRotate =
        output_ != nullptr &&
        packet.keyFrame() &&
        packet.receivedAt() - segmentOpenedAt_ >=
            config_.targetSegmentDuration;

    bool rotated = false;
    if (shouldRotate) {
        closeSegment();
        rotated = true;
    }

    if (output_ == nullptr) {
        if (!packet.keyFrame()) {
            ++stats_.packetsWaitingForKeyframe;
            return SegmentWriteResult::WaitingForKeyframe;
        }

        openSegment(packet);
    }

    PacketGuard muxPacket{
        av_packet_clone(packet.avPacket())};
    if (muxPacket.packet == nullptr) {
        throw std::bad_alloc{};
    }

    muxPacket.packet->stream_index =
        outputStream_->index;
    muxPacket.packet->pos = -1;

    if (segmentBaseTimestamp_ !=
        AV_NOPTS_VALUE) {
        if (muxPacket.packet->pts !=
            AV_NOPTS_VALUE) {
            muxPacket.packet->pts -=
                segmentBaseTimestamp_;
        }
        if (muxPacket.packet->dts !=
            AV_NOPTS_VALUE) {
            muxPacket.packet->dts -=
                segmentBaseTimestamp_;
        }
    }

    av_packet_rescale_ts(
        muxPacket.packet,
        packet.timeBase(),
        outputStream_->time_base);

    media::checkFfmpeg(
        av_interleaved_write_frame(
            output_,
            muxPacket.packet),
        "av_interleaved_write_frame(MKV)");

    ++stats_.packetsWritten;

    return rotated
        ? SegmentWriteResult::Rotated
        : SegmentWriteResult::Written;
}

void SegmentedMkvWriter::close() {
    closeSegment();
}

void SegmentedMkvWriter::openSegment(
    const media::CompressedVideoPacket& firstPacket) {
    currentPath_ = nextSegmentPath();

    AVFormatContext* output = nullptr;
    media::checkFfmpeg(
        avformat_alloc_output_context2(
            &output,
            nullptr,
            "matroska",
            currentPath_.string().c_str()),
        "avformat_alloc_output_context2(matroska)");

    if (output == nullptr) {
        throw std::runtime_error(
            "FFmpeg returned null Matroska output context");
    }

    output_ = output;

    try {
        outputStream_ =
            avformat_new_stream(output_, nullptr);
        if (outputStream_ == nullptr) {
            throw std::bad_alloc{};
        }

        media::checkFfmpeg(
            avcodec_parameters_copy(
                outputStream_->codecpar,
                descriptor_->parameters()),
            "avcodec_parameters_copy(MKV)");

        outputStream_->codecpar->codec_tag = 0;
        outputStream_->time_base =
            descriptor_->timeBase();

        if ((output_->oformat->flags &
             AVFMT_NOFILE) == 0) {
            media::checkFfmpeg(
                avio_open(
                    &output_->pb,
                    currentPath_.string().c_str(),
                    AVIO_FLAG_WRITE),
                "avio_open(MKV)");
        }

        media::checkFfmpeg(
            avformat_write_header(output_, nullptr),
            "avformat_write_header(MKV)");

        segmentOpenedAt_ =
            firstPacket.receivedAt();
        segmentBaseTimestamp_ =
            packetReferenceTimestamp(
                firstPacket.avPacket());
    } catch (...) {
        if (output_->pb != nullptr &&
            (output_->oformat->flags &
             AVFMT_NOFILE) == 0) {
            avio_closep(&output_->pb);
        }

        avformat_free_context(output_);
        output_ = nullptr;
        outputStream_ = nullptr;

        std::error_code error;
        std::filesystem::remove(
            currentPath_,
            error);
        currentPath_.clear();

        throw;
    }
}

void SegmentedMkvWriter::closeSegment() {
    if (output_ == nullptr) {
        return;
    }

    const auto closedAt =
        std::chrono::steady_clock::now();
    const auto duration =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
                closedAt - segmentOpenedAt_);

    std::exception_ptr trailerError;

    try {
        media::checkFfmpeg(
            av_write_trailer(output_),
            "av_write_trailer(MKV)");
    } catch (...) {
        trailerError = std::current_exception();
    }

    if (output_->pb != nullptr &&
        (output_->oformat->flags &
         AVFMT_NOFILE) == 0) {
        const int closeResult =
            avio_closep(&output_->pb);

        if (closeResult < 0 &&
            !trailerError) {
            try {
                media::throwFfmpegError(
                    "avio_closep(MKV)",
                    closeResult);
            } catch (...) {
                trailerError =
                    std::current_exception();
            }
        }
    }

    avformat_free_context(output_);
    output_ = nullptr;
    outputStream_ = nullptr;
    segmentBaseTimestamp_ =
        AV_NOPTS_VALUE;

    if (!currentPath_.empty()) {
        completed_.push_back(
            CompletedSegment{
                .path = currentPath_,
                .duration = duration,
            });

        completedDuration_ += duration;
        ++stats_.segmentsCompleted;
    }

    currentPath_.clear();

    enforceRetention();

    if (trailerError) {
        std::rethrow_exception(trailerError);
    }
}

void SegmentedMkvWriter::enforceRetention() {
    while (!completed_.empty() &&
           completedDuration_ >
               config_.retention) {
        const CompletedSegment oldest =
            completed_.front();

        std::error_code error;
        const bool removed =
            std::filesystem::remove(
                oldest.path,
                error);

        if (error) {
            throw std::runtime_error(
                "failed to remove expired blackbox segment: " +
                oldest.path.string() +
                ": " +
                error.message());
        }

        completedDuration_ -=
            oldest.duration;
        completed_.pop_front();

        if (removed) {
            ++stats_.segmentsDeleted;
        }
    }
}

std::filesystem::path
SegmentedMkvWriter::nextSegmentPath() {
    const auto now =
        std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
                now.time_since_epoch()) %
        1000;

    const std::time_t timestamp =
        std::chrono::system_clock::to_time_t(now);
    const std::tm local =
        localTime(timestamp);

    std::ostringstream name;
    name
        << std::put_time(
            &local,
            "%Y%m%d_%H%M%S")
        << '_'
        << std::setw(3)
        << std::setfill('0')
        << milliseconds.count()
        << '_'
        << std::setw(6)
        << std::setfill('0')
        << nextSegmentIndex_++
        << ".mkv";

    return config_.directory /
           name.str();
}

std::int64_t
SegmentedMkvWriter::packetReferenceTimestamp(
    const AVPacket* packet) noexcept {
    if (packet == nullptr) {
        return AV_NOPTS_VALUE;
    }

    if (packet->dts != AV_NOPTS_VALUE) {
        return packet->dts;
    }

    return packet->pts;
}

} // namespace reg::recorder
