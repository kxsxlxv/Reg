#include "telemetry/TelemetryModel.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

void samplesAreRateLimitedAndPruned() {
    reg::telemetry::TelemetryModel model(
        1s,
        250ms,
        16);

    const auto t0 =
        std::chrono::steady_clock::time_point{10s};

    reg::telemetry::Counters counters{};
    counters.decodedFrames = 1;
    model.updateCounters(counters, t0);

    counters.decodedFrames = 2;
    model.updateCounters(
        counters,
        t0 + 100ms);

    counters.decodedFrames = 3;
    model.updateCounters(
        counters,
        t0 + 250ms);

    auto snapshot = model.snapshot();
    require(
        snapshot.samples.size() == 2,
        "telemetry sampling interval was not enforced");
    require(
        snapshot.counters.decodedFrames == 3,
        "latest counters were not retained");

    counters.decodedFrames = 4;
    model.updateCounters(
        counters,
        t0 + 1500ms);

    snapshot = model.snapshot();
    require(
        snapshot.samples.size() == 1,
        "expired telemetry samples were not pruned");
}

void eventLogIsBoundedAndTimePruned() {
    reg::telemetry::TelemetryModel model(
        1s,
        250ms,
        2);

    const auto t0 =
        std::chrono::steady_clock::time_point{20s};

    model.log(
        reg::telemetry::Severity::Info,
        "one",
        t0);
    model.log(
        reg::telemetry::Severity::Warning,
        "two",
        t0 + 10ms);
    model.log(
        reg::telemetry::Severity::Error,
        "three",
        t0 + 20ms);

    auto snapshot = model.snapshot();
    require(
        snapshot.events.size() == 2,
        "event capacity was not enforced");
    require(
        snapshot.events.front().message == "two",
        "oldest event was not evicted");

    model.updateCounters(
        {},
        t0 + 2s);

    snapshot = model.snapshot();
    require(
        snapshot.events.empty(),
        "expired events were not pruned");
}

void targetSnapshotCopiesCurrentMetadata() {
    reg::telemetry::TelemetryModel model;

    reg::metadata::FrameMetadata metadata{};
    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = 55,
            .classId = 4,
            .flags = 2,
            .confidence = 0.75F,
            .bbox = {
                .x = 0.1F,
                .y = 0.2F,
                .width = 0.3F,
                .height = 0.4F,
            },
        });

    model.setTargets(metadata);

    auto snapshot = model.snapshot();
    require(
        snapshot.targets.size() == 1,
        "target snapshot count mismatch");
    require(
        snapshot.targets[0].id == 55,
        "target snapshot ID mismatch");
    require(
        snapshot.targets[0].classId == 4,
        "target snapshot class mismatch");

    model.clearTargets();
    snapshot = model.snapshot();
    require(
        snapshot.targets.empty(),
        "target snapshot was not cleared");
}

} // namespace

int main() {
    try {
        samplesAreRateLimitedAndPruned();
        eventLogIsBoundedAndTimePruned();
        targetSnapshotCopiesCurrentMetadata();

        std::cout
            << "telemetry_model_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "telemetry_model_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
