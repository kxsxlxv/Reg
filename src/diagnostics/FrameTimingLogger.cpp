#include "diagnostics/FrameTimingLogger.hpp"

#include "media/CompressedVideoPacket.hpp"
#include "media/FrameIdentity.hpp"
#include "video/VideoFrame.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace reg::diagnostics {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kFieldCount = 38U;
constexpr std::size_t kMaxPendingRows = 16384U;

enum Field : std::size_t {
    Event = 0,
    SteadyNs,
    WallUnixMs,
    Session,
    TimeBaseNum,
    TimeBaseDen,
    PacketPts,
    PacketDts,
    PacketDuration,
    PacketSize,
    KeyFrame,
    PacketMediaMs,
    PacketPtsDeltaMs,
    PacketArrivalDeltaMs,
    FramePts,
    FrameBestEffortPts,
    FramePktDts,
    FrameDuration,
    FrameMediaMs,
    FrameTimestampDeltaMs,
    SourceEpoch,
    SourceFrameId,
    SourceTimeNs,
    SourceDeltaMs,
    SourceFrameDelta,
    SourceMissing,
    DecodedTotal,
    DecodeArrivalDeltaMs,
    SinceLastPacketMs,
    PresentTotal,
    DecodedSincePresent,
    PresentIntervalMs,
    RenderCallMs,
    RenderNotReadyAttempts,
    RenderAttemptTotalMs,
    DecodeToPresentMs,
    PresentSourceFrameDelta,
    PresentMissing,
};

using Row = std::array<std::string, kFieldCount>;

std::int64_t steadyNs(Clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               value.time_since_epoch())
        .count();
}

std::int64_t wallUnixMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double milliseconds(Clock::duration value) noexcept {
    return std::chrono::duration<double, std::milli>(value).count();
}

std::string fixed3(double value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string csvLine(const Row& row) {
    std::string result;
    result.reserve(384U);
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0U) {
            result.push_back(',');
        }
        result += row[i];
    }
    result.push_back('\n');
    return result;
}

std::string timestampMs(
    std::int64_t timestamp,
    int timeBaseNum,
    int timeBaseDen) {
    if (timestamp == AV_NOPTS_VALUE ||
        timeBaseNum <= 0 ||
        timeBaseDen <= 0) {
        return {};
    }

    return fixed3(
        static_cast<double>(timestamp) *
        static_cast<double>(timeBaseNum) *
        1000.0 /
        static_cast<double>(timeBaseDen));
}

std::optional<double> timestampDeltaMs(
    std::int64_t current,
    std::optional<std::int64_t> previous,
    int timeBaseNum,
    int timeBaseDen) {
    if (current == AV_NOPTS_VALUE ||
        !previous.has_value() ||
        *previous == AV_NOPTS_VALUE ||
        timeBaseNum <= 0 ||
        timeBaseDen <= 0) {
        return std::nullopt;
    }

    return static_cast<double>(current - *previous) *
           static_cast<double>(timeBaseNum) *
           1000.0 /
           static_cast<double>(timeBaseDen);
}

void setIdentityFields(
    Row& row,
    const media::SourceFrameIdentity& identity,
    const std::optional<media::SourceFrameIdentity>& previous,
    Field deltaField,
    Field frameDeltaField,
    Field missingField) {
    row[SourceEpoch] = std::to_string(identity.key.streamEpoch);
    row[SourceFrameId] = std::to_string(identity.key.frameId);
    row[SourceTimeNs] = std::to_string(identity.sourceTimeNs);

    if (!previous.has_value() ||
        previous->key.streamEpoch != identity.key.streamEpoch) {
        return;
    }

    if (identity.sourceTimeNs >= previous->sourceTimeNs) {
        row[deltaField] = fixed3(
            static_cast<double>(
                identity.sourceTimeNs - previous->sourceTimeNs) /
            1'000'000.0);
    }

    if (identity.key.frameId >= previous->key.frameId) {
        const std::uint64_t frameDelta =
            identity.key.frameId - previous->key.frameId;
        row[frameDeltaField] = std::to_string(frameDelta);
        row[missingField] = std::to_string(
            frameDelta > 0U ? frameDelta - 1U : 0U);
    } else {
        // Explicit sentinel for a backwards/non-monotonic source frame ID.
        row[frameDeltaField] = "-1";
    }
}

} // namespace

struct FrameTimingLogger::Impl {
    Impl() {
        std::error_code error;
        std::filesystem::create_directories("logs", error);
        if (error) {
            std::cerr
                << "[timing] failed to create logs directory: "
                << error.message()
                << '\n';
            return;
        }

        const auto epochMs = wallUnixMs();
        path =
            std::filesystem::path{"logs"} /
            ("frame_timing_" + std::to_string(epochMs) + ".csv");

        stream.open(path, std::ios::out | std::ios::trunc);
        if (!stream.is_open()) {
            std::cerr
                << "[timing] failed to open "
                << path.string()
                << '\n';
            return;
        }

        stream
            << "event,steady_ns,wall_unix_ms,session,time_base_num,time_base_den,"
            << "packet_pts,packet_dts,packet_duration,packet_size,keyframe,"
            << "packet_media_ms,packet_pts_delta_ms,packet_arrival_delta_ms,"
            << "frame_pts,frame_best_effort_pts,frame_pkt_dts,frame_duration,"
            << "frame_media_ms,frame_timestamp_delta_ms,"
            << "source_epoch,source_frame_id,source_time_ns,source_delta_ms,"
            << "source_frame_delta,source_missing,decoded_total,"
            << "decode_arrival_delta_ms,since_last_packet_ms,present_total,"
            << "decoded_since_present,present_interval_ms,render_call_ms,"
            << "render_not_ready_attempts,render_attempt_total_ms,"
            << "decode_to_present_ms,present_source_frame_delta,present_missing\n";
        stream.flush();

        isEnabled = true;
        writer = std::thread([this] { writerLoop(); });

        std::cout
            << "[timing] frame diagnostics: "
            << path.string()
            << '\n';
    }

    ~Impl() {
        if (!isEnabled) {
            return;
        }

        {
            std::scoped_lock lock(queueMutex);
            stop = true;
        }
        queueCondition.notify_one();

        if (writer.joinable()) {
            writer.join();
        }

        stream.flush();
        stream.close();

        const auto dropped = droppedRows.load(std::memory_order_relaxed);
        if (dropped != 0U) {
            std::cerr
                << "[timing] dropped "
                << dropped
                << " diagnostic rows because the writer queue was full\n";
        }
    }

    void enqueue(Row row) {
        if (!isEnabled) {
            return;
        }

        row[WallUnixMs] = std::to_string(wallUnixMs());
        const std::string line = csvLine(row);

        std::scoped_lock lock(queueMutex);
        if (pendingRows.size() >= kMaxPendingRows) {
            droppedRows.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        pendingRows.push_back(line);
    }

    void writerLoop() {
        std::deque<std::string> local;

        while (true) {
            {
                std::unique_lock lock(queueMutex);
                queueCondition.wait_for(
                    lock,
                    std::chrono::milliseconds{250},
                    [this] { return stop; });

                pendingRows.swap(local);
                if (stop && local.empty()) {
                    break;
                }
            }

            for (const auto& line : local) {
                stream << line;
            }
            local.clear();
            stream.flush();
        }
    }

    Row baseRow(const char* eventName, Clock::time_point time) const {
        Row row{};
        row[Event] = eventName;
        row[SteadyNs] = std::to_string(steadyNs(time));
        row[Session] = std::to_string(session);
        row[TimeBaseNum] = std::to_string(timeBaseNum);
        row[TimeBaseDen] = std::to_string(timeBaseDen);
        return row;
    }

    bool isEnabled{};
    std::filesystem::path path;
    std::ofstream stream;
    std::thread writer;

    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<std::string> pendingRows;
    bool stop{};
    std::atomic_uint64_t droppedRows{0};

    std::mutex stateMutex;
    std::uint64_t session{};
    int timeBaseNum{};
    int timeBaseDen{};

    std::optional<Clock::time_point> previousPacketAt;
    std::optional<std::int64_t> previousPacketPts;
    std::optional<Clock::time_point> previousDecodeAt;
    std::optional<std::int64_t> previousFrameTimestamp;
    std::optional<media::SourceFrameIdentity> previousDecodedIdentity;

    std::optional<Clock::time_point> previousPresentAt;
    std::optional<media::SourceFrameIdentity> previousPresentIdentity;
    std::optional<std::uint64_t> decodedTotalAtPreviousPresent;
};

FrameTimingLogger::FrameTimingLogger()
    : impl_(std::make_unique<Impl>()) {}

FrameTimingLogger::~FrameTimingLogger() = default;

bool FrameTimingLogger::enabled() const noexcept {
    return impl_ != nullptr && impl_->isEnabled;
}

const std::filesystem::path& FrameTimingLogger::path() const noexcept {
    return impl_->path;
}

void FrameTimingLogger::beginSession(
    int timeBaseNum,
    int timeBaseDen) {
    if (!enabled()) {
        return;
    }

    Row row{};
    {
        std::scoped_lock lock(impl_->stateMutex);

        ++impl_->session;
        impl_->timeBaseNum = timeBaseNum;
        impl_->timeBaseDen = timeBaseDen;
        impl_->previousPacketAt.reset();
        impl_->previousPacketPts.reset();
        impl_->previousDecodeAt.reset();
        impl_->previousFrameTimestamp.reset();
        impl_->previousDecodedIdentity.reset();
        impl_->previousPresentAt.reset();
        impl_->previousPresentIdentity.reset();
        impl_->decodedTotalAtPreviousPresent.reset();

        row = impl_->baseRow("SESSION", Clock::now());
    }

    impl_->enqueue(std::move(row));
}

void FrameTimingLogger::logPacket(
    const media::CompressedVideoPacket& packet) {
    if (!enabled()) {
        return;
    }

    const AVPacket* avPacket = packet.avPacket();
    if (avPacket == nullptr) {
        return;
    }

    Row row{};
    {
        std::scoped_lock lock(impl_->stateMutex);

        const Clock::time_point now = packet.receivedAt();
        row = impl_->baseRow("PACKET", now);

        const int tbNum = packet.timeBase().num;
        const int tbDen = packet.timeBase().den;
        row[TimeBaseNum] = std::to_string(tbNum);
        row[TimeBaseDen] = std::to_string(tbDen);

        if (avPacket->pts != AV_NOPTS_VALUE) {
            row[PacketPts] = std::to_string(avPacket->pts);
            row[PacketMediaMs] =
                timestampMs(avPacket->pts, tbNum, tbDen);
        }
        if (avPacket->dts != AV_NOPTS_VALUE) {
            row[PacketDts] = std::to_string(avPacket->dts);
        }

        row[PacketDuration] = std::to_string(avPacket->duration);
        row[PacketSize] = std::to_string(avPacket->size);
        row[KeyFrame] = packet.keyFrame() ? "1" : "0";

        if (const auto delta = timestampDeltaMs(
                avPacket->pts,
                impl_->previousPacketPts,
                tbNum,
                tbDen);
            delta.has_value()) {
            row[PacketPtsDeltaMs] = fixed3(*delta);
        }

        if (impl_->previousPacketAt.has_value()) {
            row[PacketArrivalDeltaMs] = fixed3(
                milliseconds(now - *impl_->previousPacketAt));
        }

        impl_->previousPacketAt = now;
        if (avPacket->pts != AV_NOPTS_VALUE) {
            impl_->previousPacketPts = avPacket->pts;
        }
    }

    impl_->enqueue(std::move(row));
}

void FrameTimingLogger::logDecodedFrame(
    const video::VideoFrame& frame,
    std::uint64_t decodedTotal) {
    if (!enabled()) {
        return;
    }

    const AVFrame* avFrame = frame.avFrame();
    if (avFrame == nullptr) {
        return;
    }

    Row row{};
    {
        std::scoped_lock lock(impl_->stateMutex);

        const Clock::time_point now = frame.decodedAt();
        row = impl_->baseRow("DECODE", now);
        row[DecodedTotal] = std::to_string(decodedTotal);

        if (avFrame->pts != AV_NOPTS_VALUE) {
            row[FramePts] = std::to_string(avFrame->pts);
        }
        if (avFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
            row[FrameBestEffortPts] =
                std::to_string(avFrame->best_effort_timestamp);
        }
        if (avFrame->pkt_dts != AV_NOPTS_VALUE) {
            row[FramePktDts] = std::to_string(avFrame->pkt_dts);
        }
        row[FrameDuration] = std::to_string(avFrame->duration);

        const std::int64_t frameTimestamp =
            avFrame->best_effort_timestamp != AV_NOPTS_VALUE
                ? avFrame->best_effort_timestamp
                : avFrame->pts;

        if (frameTimestamp != AV_NOPTS_VALUE) {
            row[FrameMediaMs] = timestampMs(
                frameTimestamp,
                impl_->timeBaseNum,
                impl_->timeBaseDen);

            if (const auto delta = timestampDeltaMs(
                    frameTimestamp,
                    impl_->previousFrameTimestamp,
                    impl_->timeBaseNum,
                    impl_->timeBaseDen);
                delta.has_value()) {
                row[FrameTimestampDeltaMs] = fixed3(*delta);
            }
            impl_->previousFrameTimestamp = frameTimestamp;
        }

        if (impl_->previousDecodeAt.has_value()) {
            row[DecodeArrivalDeltaMs] = fixed3(
                milliseconds(now - *impl_->previousDecodeAt));
        }

        if (impl_->previousPacketAt.has_value()) {
            row[SinceLastPacketMs] = fixed3(
                milliseconds(now - *impl_->previousPacketAt));
        }

        const auto identity = frame.identity();
        if (identity.has_value()) {
            setIdentityFields(
                row,
                *identity,
                impl_->previousDecodedIdentity,
                SourceDeltaMs,
                SourceFrameDelta,
                SourceMissing);
            impl_->previousDecodedIdentity = *identity;
        }

        impl_->previousDecodeAt = now;
    }

    impl_->enqueue(std::move(row));
}

void FrameTimingLogger::logPresent(
    const video::VideoFrame& frame,
    std::uint64_t decodedTotal,
    std::uint64_t presentedTotal,
    Clock::time_point renderBegin,
    Clock::time_point renderEnd,
    std::uint64_t notReadyAttempts,
    double renderAttemptTotalMs) {
    if (!enabled()) {
        return;
    }

    Row row{};
    {
        std::scoped_lock lock(impl_->stateMutex);

        row = impl_->baseRow("PRESENT", renderEnd);
        row[DecodedTotal] = std::to_string(decodedTotal);
        row[PresentTotal] = std::to_string(presentedTotal);
        row[RenderCallMs] = fixed3(
            milliseconds(renderEnd - renderBegin));
        row[RenderNotReadyAttempts] =
            std::to_string(notReadyAttempts);
        row[RenderAttemptTotalMs] =
            fixed3(renderAttemptTotalMs);

        if (impl_->previousPresentAt.has_value()) {
            row[PresentIntervalMs] = fixed3(
                milliseconds(
                    renderEnd - *impl_->previousPresentAt));
        }

        if (impl_->decodedTotalAtPreviousPresent.has_value() &&
            decodedTotal >=
                *impl_->decodedTotalAtPreviousPresent) {
            row[DecodedSincePresent] = std::to_string(
                decodedTotal -
                *impl_->decodedTotalAtPreviousPresent);
        }

        if (renderEnd >= frame.decodedAt()) {
            row[DecodeToPresentMs] = fixed3(
                milliseconds(renderEnd - frame.decodedAt()));
        }

        const auto identity = frame.identity();
        if (identity.has_value()) {
            setIdentityFields(
                row,
                *identity,
                impl_->previousPresentIdentity,
                SourceDeltaMs,
                PresentSourceFrameDelta,
                PresentMissing);
            impl_->previousPresentIdentity = *identity;
        }

        impl_->previousPresentAt = renderEnd;
        impl_->decodedTotalAtPreviousPresent = decodedTotal;
    }

    impl_->enqueue(std::move(row));
}

} // namespace reg::diagnostics
