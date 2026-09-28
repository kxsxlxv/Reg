#include "telemetry/TelemetryModel.hpp"

#include <stdexcept>
#include <utility>

namespace reg::telemetry {

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

void TelemetryModel::clearTargets() {
    std::scoped_lock lock(mutex_);
    targets_.clear();
}

void TelemetryModel::log(
    Severity severity,
    std::string message,
    std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex_);

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
    std::scoped_lock lock(mutex_);

    return Snapshot{
        .counters = counters_,
        .samples = std::vector<Sample>(
            samples_.begin(),
            samples_.end()),
        .events = std::vector<Event>(
            events_.begin(),
            events_.end()),
        .targets = targets_,
    };
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
