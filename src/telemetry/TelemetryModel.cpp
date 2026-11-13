#include "telemetry/TelemetryModel.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace reg::telemetry {
namespace {

constexpr std::string_view kRtspFailurePrefix =
    "RTSP session failed: ";

bool isExpectedRtspWaitFailure(std::string_view message) noexcept {
    if (!message.starts_with(kRtspFailurePrefix)) {
        return false;
    }

    return message.find("Server returned 404 Not Found") !=
               std::string_view::npos ||
           message.find("End of file") !=
               std::string_view::npos;
}

} // namespace

TelemetryModel::TelemetryModel(
    std::chrono::milliseconds retention,
    std::chrono::milliseconds sampleInterval,
    std::size_t maxEvents)
    : retention_(retention),
      sampleInterval_(sampleInterval),
      maxEvents_(maxEvents) {
    if (retention_ <=
        std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "telemetry retention must be positive");
    }
    if (sampleInterval_ <=
        std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "telemetry sample interval must be positive");
    }
    if (maxEvents_ == 0) {
        throw std::invalid_argument(
            "telemetry event capacity must be positive");
    }
}

void TelemetryModel::updateCounters(
    Counters counters,
    std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);

    counters_ = counters;

    if (lastSampleTime_ ==
            std::chrono::steady_clock::time_point{} ||
        now - lastSampleTime_ >=
            sampleInterval_) {
        samples_.push_back(
            Sample{
                .time = now,
                .counters = counters_,
            });
        lastSampleTime_ = now;
    }

    pruneLocked(now);
}

void TelemetryModel::setTargets(
    const metadata::FrameMetadata& metadata) {
    std::scoped_lock lock(mutex_);

    targets_.clear();
    targets_.reserve(
        metadata.targets.size());

    for (const auto& target :
         metadata.targets) {
        targets_.push_back(
            Target{
                .id = target.id,
                .classId = target.classId,
                .flags = target.flags,
                .confidence = target.confidence,
                .bbox = target.bbox,
            });
    }
}

void TelemetryModel::observeGtRange(
    const media::GtMetadata& gt,
    std::chrono::steady_clock::time_point when) {
    // Best-effort UI data; never stall the decoder for a diagnostic chart.
    std::unique_lock lock(gtRangeMutex_, std::try_to_lock);
    if (lock.owns_lock())
        gtRanges_.observeGt(gt, when);
}

void TelemetryModel::observeCvForGt(
    const metadata::FrameMetadata& metadata) {
    // Accepted CVM1 remains in its authoritative store even if UI skips it.
    std::unique_lock lock(gtRangeMutex_, std::try_to_lock);
    if (lock.owns_lock())
        gtRanges_.observeCv(metadata);
}

void TelemetryModel::resetGtRanges() {
    std::scoped_lock lock(gtRangeMutex_);
    gtRanges_.clear();
}

void TelemetryModel::clearTargets() {
    std::scoped_lock lock(mutex_);
    targets_.clear();
}

void TelemetryModel::log(
    Severity severity,
    std::string message,
    std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);

    // During startup/reconnect, MediaMTX can legitimately return 404 until
    // the publisher creates the RTSP path, and a just-created path can end
    // immediately before the first frame arrives. These are retry states,
    // not application faults. Keep the operational signal-loss warning as
    // the single WARN once video had actually been established.
    if (severity == Severity::Warning &&
        isExpectedRtspWaitFailure(message)) {
        severity = Severity::Info;
        message =
            std::string("Ожидание RTSP: ") +
            message.substr(kRtspFailurePrefix.size());
    }

    if (severity == Severity::Warning &&
        counters_.decodedFrames == 0U &&
        message ==
            "NO SIGNAL: decoded video is not arriving") {
        severity = Severity::Info;
        message = "Ожидание видеосигнала";
    }

    events_.push_back(
        Event{
            .time = now,
            .severity = severity,
            .message = std::move(message),
        });

    while (events_.size() > maxEvents_) {
        events_.pop_front();
    }

    pruneLocked(now);
}

Snapshot TelemetryModel::snapshot() const {
    Snapshot snapshot;
    {
        std::scoped_lock lock(mutex_);
        snapshot.counters = counters_;
        snapshot.samples.assign(samples_.begin(), samples_.end());
        snapshot.events.assign(events_.begin(), events_.end());
        snapshot.targets = targets_;
    }
    {
        std::scoped_lock lock(gtRangeMutex_);
        snapshot.gtRangeTracks =
            gtRanges_.snapshot(std::chrono::steady_clock::now());
    }
    return snapshot;
}

void TelemetryModel::pruneLocked(
    std::chrono::steady_clock::time_point now) {
    const auto cutoff = now - retention_;

    while (!samples_.empty() &&
           samples_.front().time < cutoff) {
        samples_.pop_front();
    }

    while (!events_.empty() &&
           events_.front().time < cutoff) {
        events_.pop_front();
    }
}

const char* toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info:
        return "INFO";
    case Severity::Warning:
        return "WARN";
    case Severity::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace reg::telemetry
