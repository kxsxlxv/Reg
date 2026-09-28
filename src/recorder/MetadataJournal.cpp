#include "recorder/MetadataJournal.hpp"

#include "metadata/Protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace reg::recorder {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{
    'C', 'V', 'M', 'J'};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kHeaderSize = 32;
constexpr std::uint32_t kRecordHeaderSize = 16;

void writeLe16(
    std::array<std::uint8_t, kHeaderSize>& bytes,
    std::size_t offset,
    std::uint16_t value) {
    bytes[offset] =
        static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] =
        static_cast<std::uint8_t>(
            (value >> 8U) & 0xffU);
}

void writeLe64(
    std::array<std::uint8_t, kHeaderSize>& bytes,
    std::size_t offset,
    std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

void writeLe32(
    std::array<std::uint8_t, kRecordHeaderSize>& bytes,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

void writeLe64(
    std::array<std::uint8_t, kRecordHeaderSize>& bytes,
    std::size_t offset,
    std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>(
                (value >> (8U * i)) & 0xffU);
    }
}

std::uint16_t readLe16(
    const std::array<std::uint8_t, kHeaderSize>& bytes,
    std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                bytes[offset + 1]) << 8U);
}

std::uint64_t readLe64(
    const std::array<std::uint8_t, kHeaderSize>& bytes,
    std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::uint32_t readLe32(
    const std::array<std::uint8_t, kRecordHeaderSize>& bytes,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |=
            static_cast<std::uint32_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::uint64_t readLe64(
    const std::array<std::uint8_t, kRecordHeaderSize>& bytes,
    std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::tm localTime(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::uint64_t systemNowNs() {
    const auto now =
        std::chrono::system_clock::now()
            .time_since_epoch();

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(now)
            .count());
}

std::chrono::steady_clock::time_point
effectiveReceivedAt(
    const metadata::FrameMetadata& metadata) {
    if (metadata.receivedAt ==
        std::chrono::steady_clock::time_point{}) {
        return std::chrono::steady_clock::now();
    }
    return metadata.receivedAt;
}

void writeBytes(
    std::ofstream& stream,
    const void* data,
    std::size_t size,
    const char* operation) {
    stream.write(
        static_cast<const char*>(data),
        static_cast<std::streamsize>(size));

    if (!stream) {
        throw std::runtime_error(operation);
    }
}

void readBytes(
    std::ifstream& stream,
    void* data,
    std::size_t size,
    const char* operation) {
    stream.read(
        static_cast<char*>(data),
        static_cast<std::streamsize>(size));

    if (!stream) {
        throw std::runtime_error(operation);
    }
}

} // namespace

MetadataJournalWriter::MetadataJournalWriter(
    MetadataJournalConfig config)
    : config_(std::move(config)) {
    if (config_.targetSegmentDuration <=
        std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "metadata journal segment duration must be positive");
    }
    if (config_.retention <
        config_.targetSegmentDuration) {
        throw std::invalid_argument(
            "metadata journal retention must be at least one segment");
    }

    std::filesystem::create_directories(
        config_.directory);
}

MetadataJournalWriter::~MetadataJournalWriter() {
    try {
        close();
    } catch (...) {
    }
}

void MetadataJournalWriter::write(
    const metadata::FrameMetadata& metadata) {
    const auto receivedAt =
        effectiveReceivedAt(metadata);

    const bool epochChanged =
        stream_.is_open() &&
        metadata.key.streamEpoch != currentEpoch_;

    const bool durationElapsed =
        stream_.is_open() &&
        receivedAt - segmentOpenedAt_ >=
            config_.targetSegmentDuration;

    if (epochChanged || durationElapsed) {
        closeSegment();
    }

    if (!stream_.is_open()) {
        openSegment(metadata, receivedAt);
    }

    const auto payload =
        metadata::protocol::encodeFrameMetadata(
            metadata);

    const auto offset =
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                receivedAt - segmentOpenedAt_);

    const auto offsetCount =
        std::max<std::int64_t>(
            0,
            offset.count());

    if (payload.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "metadata journal payload too large");
    }

    std::array<
        std::uint8_t,
        kRecordHeaderSize>
        recordHeader{};

    writeLe64(
        recordHeader,
        0,
        static_cast<std::uint64_t>(
            offsetCount));

    writeLe32(
        recordHeader,
        8,
        static_cast<std::uint32_t>(
            payload.size()));

    writeLe32(
        recordHeader,
        12,
        0);

    writeBytes(
        stream_,
        recordHeader.data(),
        recordHeader.size(),
        "failed to write metadata journal record header");

    writeBytes(
        stream_,
        payload.data(),
        payload.size(),
        "failed to write metadata journal payload");

    lastRecordAt_ = receivedAt;
    ++stats_.recordsWritten;
}

void MetadataJournalWriter::close() {
    closeSegment();
}

void MetadataJournalWriter::openSegment(
    const metadata::FrameMetadata& metadata,
    std::chrono::steady_clock::time_point receivedAt) {
    currentEpoch_ = metadata.key.streamEpoch;
    currentPath_ =
        nextSegmentPath(currentEpoch_);
    segmentOpenedAt_ = receivedAt;
    lastRecordAt_ = receivedAt;
    segmentStartUnixNs_ = systemNowNs();

    stream_.open(
        currentPath_,
        std::ios::binary |
            std::ios::out |
            std::ios::trunc);

    if (!stream_) {
        throw std::runtime_error(
            "failed to open metadata journal: " +
            currentPath_.string());
    }

    std::array<std::uint8_t, kHeaderSize>
        header{};

    std::copy(
        kMagic.begin(),
        kMagic.end(),
        header.begin());

    writeLe16(
        header,
        4,
        kVersion);
    writeLe16(
        header,
        6,
        kHeaderSize);
    writeLe64(
        header,
        8,
        segmentStartUnixNs_);
    writeLe64(
        header,
        16,
        currentEpoch_);
    writeLe64(
        header,
        24,
        0);

    writeBytes(
        stream_,
        header.data(),
        header.size(),
        "failed to write metadata journal header");
}

void MetadataJournalWriter::closeSegment() {
    if (!stream_.is_open()) {
        return;
    }

    stream_.flush();
    if (!stream_) {
        stream_.close();
        throw std::runtime_error(
            "failed to flush metadata journal");
    }

    stream_.close();

    const auto duration =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
                lastRecordAt_ - segmentOpenedAt_);

    completed_.push_back(
        CompletedSegment{
            .path = currentPath_,
            .duration =
                std::max(
                    duration,
                    std::chrono::milliseconds{1}),
        });

    completedDuration_ +=
        completed_.back().duration;

    ++stats_.segmentsCompleted;

    currentPath_.clear();
    currentEpoch_ = 0;
    segmentStartUnixNs_ = 0;

    enforceRetention();
}

void MetadataJournalWriter::enforceRetention() {
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
                "failed to remove expired metadata journal: " +
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
MetadataJournalWriter::nextSegmentPath(
    std::uint64_t streamEpoch) {
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
        << "metadata_"
        << std::put_time(
            &local,
            "%Y%m%d_%H%M%S")
        << '_'
        << std::setw(3)
        << std::setfill('0')
        << milliseconds.count()
        << "_e"
        << std::hex
        << streamEpoch
        << std::dec
        << '_'
        << std::setw(6)
        << std::setfill('0')
        << nextSegmentIndex_++
        << ".cvmj";

    return config_.directory /
           name.str();
}

MetadataJournalFile readMetadataJournalFile(
    const std::filesystem::path& path) {
    std::ifstream stream(
        path,
        std::ios::binary);

    if (!stream) {
        throw std::runtime_error(
            "failed to open metadata journal for reading: " +
            path.string());
    }

    std::array<std::uint8_t, kHeaderSize>
        header{};

    readBytes(
        stream,
        header.data(),
        header.size(),
        "metadata journal header is truncated");

    if (!std::equal(
            kMagic.begin(),
            kMagic.end(),
            header.begin())) {
        throw std::runtime_error(
            "metadata journal has invalid magic");
    }

    const auto version =
        readLe16(header, 4);
    const auto headerSize =
        readLe16(header, 6);

    if (version != kVersion ||
        headerSize != kHeaderSize) {
        throw std::runtime_error(
            "metadata journal version is unsupported");
    }

    MetadataJournalFile result{
        .streamEpoch =
            readLe64(header, 16),
        .segmentStartUnixNs =
            readLe64(header, 8),
    };

    while (true) {
        std::array<
            std::uint8_t,
            kRecordHeaderSize>
            recordHeader{};

        stream.read(
            reinterpret_cast<char*>(
                recordHeader.data()),
            static_cast<std::streamsize>(
                recordHeader.size()));

        if (stream.eof() &&
            stream.gcount() == 0) {
            break;
        }

        if (!stream) {
            throw std::runtime_error(
                "metadata journal record header is truncated");
        }

        const std::uint64_t offsetNs =
            readLe64(recordHeader, 0);
        const std::uint32_t payloadSize =
            readLe32(recordHeader, 8);

        if (payloadSize == 0 ||
            payloadSize >
                metadata::protocol::kMaxDatagramSize) {
            throw std::runtime_error(
                "metadata journal payload size is invalid");
        }

        std::vector<std::uint8_t>
            payload(payloadSize);

        readBytes(
            stream,
            payload.data(),
            payload.size(),
            "metadata journal payload is truncated");

        const auto decoded =
            metadata::protocol::decodeFrameMetadata(
                payload);

        if (!decoded) {
            throw std::runtime_error(
                std::string(
                    "metadata journal contains invalid CVM1 packet: ") +
                metadata::protocol::toString(
                    decoded.error));
        }

        if (decoded.metadata.key.streamEpoch !=
            result.streamEpoch) {
            throw std::runtime_error(
                "metadata journal record epoch differs from file epoch");
        }

        result.records.push_back(
            MetadataJournalRecord{
                .receiveOffset =
                    std::chrono::nanoseconds{
                        static_cast<
                            std::int64_t>(
                                std::min<
                                    std::uint64_t>(
                                    offsetNs,
                                    static_cast<
                                        std::uint64_t>(
                                        std::numeric_limits<
                                            std::int64_t>::max())))},
                .metadata =
                    decoded.metadata,
            });
    }

    return result;
}

} // namespace reg::recorder
