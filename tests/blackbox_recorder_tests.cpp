#include "media/CompressedVideoPacket.hpp"
#include "media/VideoStreamDescriptor.hpp"
#include "metadata/FrameMetadata.hpp"
#include "recorder/MetadataJournal.hpp"
#include "recorder/SegmentedMkvWriter.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct CodecParametersDeleter {
    void operator()(AVCodecParameters* parameters) const noexcept {
        avcodec_parameters_free(&parameters);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

using ParametersPtr =
    std::unique_ptr<
        AVCodecParameters,
        CodecParametersDeleter>;

using PacketPtr =
    std::unique_ptr<
        AVPacket,
        PacketDeleter>;

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

reg::media::VideoStreamDescriptorPtr
makeDescriptor() {
    ParametersPtr parameters(
        avcodec_parameters_alloc());

    if (!parameters) {
        throw std::bad_alloc{};
    }

    parameters->codec_type =
        AVMEDIA_TYPE_VIDEO;
    parameters->codec_id =
        AV_CODEC_ID_H264;
    parameters->width = 640;
    parameters->height = 360;
    parameters->profile =
        AV_PROFILE_H264_BASELINE;
    parameters->level = 31;

    // AVCDecoderConfigurationRecord. The writer does not decode this data;
    // it must preserve valid H.264 codec private data in the MKV header.
    constexpr std::array<std::uint8_t, 29>
        avcc{
            0x01, 0x42, 0x00, 0x1f,
            0xff, 0xe1, 0x00, 0x0e,
            0x67, 0x42, 0x00, 0x1f,
            0xe5, 0x88, 0x68, 0x54,
            0x05, 0x01, 0xed, 0x00,
            0xf0, 0x88,
            0x01, 0x00, 0x04,
            0x68, 0xce, 0x06, 0xe2,
        };

    parameters->extradata =
        static_cast<std::uint8_t*>(
            av_mallocz(
                avcc.size() +
                AV_INPUT_BUFFER_PADDING_SIZE));

    if (parameters->extradata == nullptr) {
        throw std::bad_alloc{};
    }

    parameters->extradata_size =
        static_cast<int>(avcc.size());

    std::copy(
        avcc.begin(),
        avcc.end(),
        parameters->extradata);

    return reg::media::VideoStreamDescriptor::cloneFrom(
        parameters.get(),
        AVRational{1, 90000});
}

reg::media::CompressedVideoPacketPtr
makePacket(
    std::int64_t timestamp,
    bool keyFrame,
    std::chrono::steady_clock::time_point receivedAt) {
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        throw std::bad_alloc{};
    }

    constexpr std::array<std::uint8_t, 6>
        keyBytes{
            0x00, 0x00, 0x00, 0x02,
            0x65, 0x88,
        };

    constexpr std::array<std::uint8_t, 6>
        deltaBytes{
            0x00, 0x00, 0x00, 0x02,
            0x41, 0x9a,
        };

    const auto& bytes =
        keyFrame ? keyBytes : deltaBytes;

    if (av_new_packet(
            packet.get(),
            static_cast<int>(bytes.size())) < 0) {
        throw std::bad_alloc{};
    }

    std::copy(
        bytes.begin(),
        bytes.end(),
        packet->data);

    packet->pts = timestamp;
    packet->dts = timestamp;
    packet->duration = 1500;
    packet->flags =
        keyFrame ? AV_PKT_FLAG_KEY : 0;

    return reg::media::CompressedVideoPacket::cloneFrom(
        packet.get(),
        AVRational{1, 90000},
        receivedAt);
}

reg::metadata::FrameMetadata makeMetadata(
    std::uint64_t epoch,
    std::uint64_t frameId,
    std::uint32_t sequence,
    std::chrono::steady_clock::time_point receivedAt) {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {
        .streamEpoch = epoch,
        .frameId = frameId,
    };
    metadata.sequence = sequence;
    metadata.flags = 3;
    metadata.cvBeginNs = 1000 + frameId;
    metadata.cvEndNs = 2000 + frameId;
    metadata.receivedAt = receivedAt;
    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = 42,
            .classId = 7,
            .flags = 1,
            .confidence = 0.875F,
            .bbox = {
                .x = 0.1F,
                .y = 0.2F,
                .width = 0.3F,
                .height = 0.4F,
            },
        });
    return metadata;
}

void requireReadableMatroska(
    const std::filesystem::path& path) {
    AVFormatContext* input = nullptr;

    const int result =
        avformat_open_input(
            &input,
            path.string().c_str(),
            nullptr,
            nullptr);

    if (result < 0) {
        throw std::runtime_error(
            "recorded MKV cannot be opened");
    }

    require(
        input->nb_streams == 1,
        "recorded MKV must contain one stream");

    require(
        input->streams[0]->codecpar->codec_id ==
            AV_CODEC_ID_H264,
        "recorded MKV did not preserve H.264 codec ID");

    avformat_close_input(&input);
}

void writerStartsOnKeyframeAndRotates() {
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    const auto directory =
        std::filesystem::temp_directory_path() /
        ("reg_blackbox_test_" + unique);

    std::filesystem::remove_all(directory);

    const auto base =
        std::chrono::steady_clock::now() -
        500ms;

    try {
        reg::recorder::SegmentedMkvWriter writer(
            reg::recorder::SegmentWriterConfig{
                .directory = directory,
                .targetSegmentDuration = 50ms,
                .retention = 2s,
            });

        writer.setStreamDescriptor(
            makeDescriptor());

        const auto waiting =
            writer.write(
                *makePacket(
                    0,
                    false,
                    base));

        require(
            waiting ==
                reg::recorder::SegmentWriteResult::
                    WaitingForKeyframe,
            "writer must wait for keyframe");

        const auto first =
            writer.write(
                *makePacket(
                    0,
                    true,
                    base + 10ms));

        require(
            first ==
                reg::recorder::SegmentWriteResult::Written,
            "first keyframe did not open segment");

        static_cast<void>(
            writer.write(
                *makePacket(
                    3000,
                    false,
                    base + 30ms)));

        const auto rotated =
            writer.write(
                *makePacket(
                    9000,
                    true,
                    base + 80ms));

        require(
            rotated ==
                reg::recorder::SegmentWriteResult::Rotated,
            "eligible keyframe did not rotate segment");

        writer.close();

        const auto stats = writer.stats();

        require(
            stats.packetsWaitingForKeyframe == 1,
            "waiting-keyframe statistic mismatch");
        require(
            stats.packetsWritten == 3,
            "written packet statistic mismatch");
        require(
            stats.segmentsCompleted == 2,
            "expected two completed segments");

        std::vector<std::filesystem::path> segments;
        for (const auto& entry :
             std::filesystem::directory_iterator(
                 directory)) {
            if (entry.path().extension() == ".mkv") {
                segments.push_back(entry.path());
            }
        }

        require(
            segments.size() == 2,
            "expected two MKV files");

        for (const auto& segment : segments) {
            require(
                std::filesystem::file_size(segment) > 0,
                "MKV segment is empty");
            requireReadableMatroska(segment);
        }
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}

void metadataJournalRoundTripsAndRotates() {
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    const auto directory =
        std::filesystem::temp_directory_path() /
        ("reg_metadata_journal_test_" + unique);

    std::filesystem::remove_all(directory);

    const auto base =
        std::chrono::steady_clock::now();

    try {
        reg::recorder::MetadataJournalWriter writer(
            reg::recorder::MetadataJournalConfig{
                .directory = directory,
                .targetSegmentDuration = 50ms,
                .retention = 2s,
            });

        writer.write(makeMetadata(9, 100, 1, base));
        writer.write(makeMetadata(9, 101, 2, base + 20ms));
        writer.write(makeMetadata(9, 102, 3, base + 80ms));
        writer.close();

        const auto stats = writer.stats();
        require(
            stats.recordsWritten == 3,
            "metadata journal written record count mismatch");
        require(
            stats.segmentsCompleted == 2,
            "metadata journal must rotate into two segments");

        std::vector<std::filesystem::path> journals;
        for (const auto& entry :
             std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".cvmj") {
                journals.push_back(entry.path());
            }
        }

        std::sort(journals.begin(), journals.end());

        require(
            journals.size() == 2,
            "expected two metadata journal files");

        std::vector<reg::recorder::MetadataJournalRecord>
            records;

        for (const auto& journalPath : journals) {
            const auto journal =
                reg::recorder::readMetadataJournalFile(
                    journalPath);

            require(
                journal.streamEpoch == 9,
                "metadata journal epoch mismatch");

            records.insert(
                records.end(),
                journal.records.begin(),
                journal.records.end());
        }

        require(
            records.size() == 3,
            "metadata journal replay record count mismatch");

        require(
            records[0].metadata.key ==
                reg::media::FrameKey{9, 100},
            "metadata journal first FrameKey mismatch");
        require(
            records[1].metadata.key ==
                reg::media::FrameKey{9, 101},
            "metadata journal second FrameKey mismatch");
        require(
            records[2].metadata.key ==
                reg::media::FrameKey{9, 102},
            "metadata journal third FrameKey mismatch");

        require(
            records[0].metadata.targets.size() == 1 &&
                records[0].metadata.targets[0].id == 42,
            "metadata journal target payload mismatch");

        require(
            records[1].receiveOffset == 20ms,
            "metadata journal receive offset mismatch");
        require(
            records[2].receiveOffset == 0ns,
            "new metadata journal segment must restart receive offset");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    try {
        writerStartsOnKeyframeAndRotates();
        metadataJournalRoundTripsAndRotates();

        std::cout
            << "blackbox_recorder_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "blackbox_recorder_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
