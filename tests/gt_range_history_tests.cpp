#include "telemetry/GtRangeHistory.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

reg::media::GtMetadata gt(
    std::uint64_t frameId, double meters,
    std::chrono::steady_clock::time_point /*t*/,
    std::uint64_t epoch = 7) {
    reg::media::GtMetadata frame;
    frame.status = reg::media::GtStatus::Ready;
    frame.source.key = {epoch, frameId};
    frame.source.sourceTimeNs = frameId * 1000000U;
    frame.videoWidth = 1920;
    frame.videoHeight = 1080;
    reg::media::GtObject object;
    object.id = "npc-01";
    object.visible = true;
    object.validBox = true;
    object.xyxyVideo = {500, 300, 700, 450};
    object.rangeM = meters;
    object.validRange = true;
    frame.objects.push_back(object);
    return frame;
}

reg::metadata::FrameMetadata cv(
    std::uint64_t frameId,
    std::chrono::steady_clock::time_point when,
    std::uint64_t epoch = 7) {
    reg::metadata::FrameMetadata metadata;
    metadata.key = {epoch, frameId};
    metadata.receivedAt = when;
    reg::metadata::TargetMetadata target;
    target.id = 42;
    target.flags = 1;
    target.classId = 3;
    target.confidence = 0.9F;
    target.bbox = {
        500.0F / 1920.0F, 300.0F / 1080.0F,
        200.0F / 1920.0F, 150.0F / 1080.0F
    };
    metadata.targets.push_back(target);
    return metadata;
}

void sparseGtAndNoFakeZeros() {
    reg::telemetry::GtRangeHistory history;
    const auto base = std::chrono::steady_clock::now();
    history.observeGt(gt(1, 800.0, base), base);
    reg::media::GtMetadata missing;
    missing.status = reg::media::GtStatus::Missing;
    history.observeGt(missing, base + 100ms);
    history.observeGt(gt(2, 797.5, base + 200ms), base + 200ms);
    auto rows = history.snapshot(base + 200ms);
    require(rows.size() == 1, "GT range track missing");
    require(rows[0].points.size() == 2, "missing GT invented a point");
    require(rows[0].points[0].meters == 800.0 &&
            rows[0].points[1].meters == 797.5,
            "GT range was interpolated or forced to zero");

    history.observeGt(gt(3, 0.0, base + 300ms), base + 300ms);
    rows = history.snapshot(base + 300ms);
    require(rows[0].points.back().meters == 0.0,
            "a real zero measurement must remain valid");
}

void exactFrameJoinBothArrivalOrders() {
    reg::telemetry::GtRangeHistory history;
    const auto base = std::chrono::steady_clock::now();
    history.observeGt(gt(10, 600.0, base), base);
    history.observeCv(cv(11, base + 10ms)); // different FrameKey
    auto rows = history.snapshot(base + 10ms);
    require(rows.size() == 1 && rows[0].matches.empty(),
            "CV was attached to the wrong frame");

    history.observeCv(cv(10, base + 20ms));
    rows = history.snapshot(base + 20ms);
    require(rows[0].matches.size() == 1, "GT-first exact-frame CV not joined");
    require(rows[0].matches[0].frame.frameId == 10 &&
            rows[0].matches[0].meters == 600.0 &&
            rows[0].matches[0].iou > 0.99,
            "joined CV lost exact GT measurement");
    history.observeCv(cv(10, base + 30ms));
    rows = history.snapshot(base + 30ms);
    require(rows[0].matches.size() == 1,
            "duplicate CV created a phantom hit");

    history.observeCv(cv(20, base + 40ms));
    history.observeGt(gt(20, 500.0, base + 50ms), base + 50ms);
    rows = history.snapshot(base + 50ms);
    require(rows[0].matches.size() == 2, "CV-first exact-frame join failed");
    require(rows[0].matches[1].meters == 500.0,
            "CV-first join used stale range");
    require(rows[0].matches[1].time == base + 50ms,
            "CV marker must use GT sample time, not CV packet arrival");
    require(history.diagnostics().joinedFrames == 2 &&
            history.diagnostics().matchedTargets == 2,
            "exact joins/matched targets diagnostics wrong");

    history.observeCv(cv(21, base + 60ms, 99));
    history.observeGt(gt(21, 480.0, base + 80ms, 7), base + 80ms);
    rows = history.snapshot(base + 80ms);
    require(rows[0].matches.size() == 2,
            "different stream epochs were joined");
}

void lowIouIgnoredAndReset() {
    reg::telemetry::GtRangeHistory history;
    const auto base = std::chrono::steady_clock::now();
    auto cvFrame = cv(30, base);
    cvFrame.targets[0].bbox = {0.8F, 0.8F, 0.1F, 0.1F};
    history.observeCv(cvFrame);
    history.observeGt(gt(30, 300.0, base + 20ms), base + 20ms);
    auto rows = history.snapshot(base + 20ms);
    require(rows.size() == 1 && rows[0].matches.empty(),
            "unrelated bounding boxes were falsely matched");
    require(history.diagnostics().unmatchedCvTargets == 1,
            "unmatched CV targets should be accounted for");
    history.clear();
    require(history.snapshot(base + 20ms).empty(), "session reset kept GT ID");
}
} // namespace

int main() {
    try {
        sparseGtAndNoFakeZeros();
        exactFrameJoinBothArrivalOrders();
        lowIouIgnoredAndReset();
        std::cout << "gt range history: OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gt range history: " << e.what() << '\n';
        return 1;
    }
}
