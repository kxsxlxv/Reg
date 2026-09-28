#include "metadata/FrameMetadata.hpp"
#include "overlay/PatternedLine.hpp"
#include "overlay/TrackHistory.hpp"
#include "video/VideoTransform.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void requireNear(float actual, float expected, float epsilon, std::string_view message) {
    if (std::abs(actual - expected) > epsilon) {
        throw std::runtime_error(
            std::string(message) +
            ": expected " + std::to_string(expected) +
            ", got " + std::to_string(actual));
    }
}

void requireVecNear(
    reg::overlay::Vec2f actual,
    reg::overlay::Vec2f expected,
    float epsilon,
    std::string_view message) {
    requireNear(actual.x, expected.x, epsilon, std::string(message) + " x");
    requireNear(actual.y, expected.y, epsilon, std::string(message) + " y");
}

void testContainTransform() {
    reg::video::VideoTransform transform({
        .sourceWidth = 1920,
        .sourceHeight = 1080,
        .targetWidth = 1000.0F,
        .targetHeight = 1000.0F,
        .fitMode = reg::video::FitMode::Contain,
    });

    const auto center = transform.normalizedToScreen({0.5F, 0.5F});
    requireVecNear(center, {500.0F, 500.0F}, 0.001F, "contain center");

    const auto content = transform.sourceContentRectOnScreen();
    requireNear(content.x, 0.0F, 0.001F, "contain x");
    requireNear(content.width, 1000.0F, 0.001F, "contain width");
    requireNear(content.height, 562.5F, 0.001F, "contain height");
    requireNear(content.y, 218.75F, 0.001F, "contain letterbox y");
}

void testCoverTransform() {
    reg::video::VideoTransform transform({
        .sourceWidth = 1920,
        .sourceHeight = 1080,
        .targetWidth = 1000.0F,
        .targetHeight = 1000.0F,
        .fitMode = reg::video::FitMode::Cover,
    });

    const auto content = transform.sourceContentRectOnScreen();
    requireNear(content.height, 1000.0F, 0.001F, "cover height");
    requireNear(content.width, 1777.7778F, 0.01F, "cover width");
    requireNear(content.x, -388.8889F, 0.01F, "cover crop x");
    requireNear(content.y, 0.0F, 0.001F, "cover y");
}

void testZoomPanAndInverse() {
    reg::video::VideoTransform transform({
        .sourceWidth = 1920,
        .sourceHeight = 1080,
        .targetWidth = 1280.0F,
        .targetHeight = 720.0F,
        .fitMode = reg::video::FitMode::Contain,
        .zoom = 2.0F,
        .centerNormalized = {0.25F, 0.60F},
    });

    requireVecNear(
        transform.normalizedToScreen({0.25F, 0.60F}),
        {640.0F, 360.0F},
        0.001F,
        "pan center");

    const reg::overlay::Vec2f source{0.73F, 0.19F};
    const auto screen = transform.normalizedToScreen(source);
    const auto roundTrip = transform.screenToNormalized(screen);
    requireVecNear(roundTrip, source, 0.00001F, "transform inverse");
}

void testBoundingBoxTransform() {
    reg::video::VideoTransform transform({
        .sourceWidth = 1920,
        .sourceHeight = 1080,
        .targetWidth = 1920.0F,
        .targetHeight = 1080.0F,
    });

    const auto box = transform.normalizedRectToScreen({
        .x = 0.25F,
        .y = 0.20F,
        .width = 0.50F,
        .height = 0.40F,
    });

    requireNear(box.x, 480.0F, 0.001F, "bbox x");
    requireNear(box.y, 216.0F, 0.001F, "bbox y");
    requireNear(box.width, 960.0F, 0.001F, "bbox width");
    requireNear(box.height, 432.0F, 0.001F, "bbox height");
}

void testDashedLine() {
    const auto segments = reg::overlay::tessellatePatternedLine(
        {0.0F, 0.0F},
        {30.0F, 0.0F},
        reg::overlay::StrokePattern{
            .pattern = reg::overlay::LinePattern::Dashed,
            .dashLength = 8.0F,
            .gapLength = 4.0F,
        });

    require(segments.size() == 3, "dashed line segment count");
    requireVecNear(segments[0].start, {0.0F, 0.0F}, 0.001F, "dash 0 start");
    requireVecNear(segments[0].end, {8.0F, 0.0F}, 0.001F, "dash 0 end");
    requireVecNear(segments[1].start, {12.0F, 0.0F}, 0.001F, "dash 1 start");
    requireVecNear(segments[1].end, {20.0F, 0.0F}, 0.001F, "dash 1 end");
    requireVecNear(segments[2].start, {24.0F, 0.0F}, 0.001F, "dash 2 start");
    requireVecNear(segments[2].end, {30.0F, 0.0F}, 0.001F, "dash 2 end");
}

void testDashDotLine() {
    const auto segments = reg::overlay::tessellatePatternedLine(
        {0.0F, 0.0F},
        {30.0F, 0.0F},
        reg::overlay::StrokePattern{
            .pattern = reg::overlay::LinePattern::DashDot,
            .dashLength = 8.0F,
            .gapLength = 4.0F,
            .dotLength = 2.0F,
        });

    require(segments.size() == 3, "dash-dot segment count");
    requireVecNear(segments[0].start, {0.0F, 0.0F}, 0.001F, "dash-dot 0 start");
    requireVecNear(segments[0].end, {8.0F, 0.0F}, 0.001F, "dash-dot 0 end");
    requireVecNear(segments[1].start, {12.0F, 0.0F}, 0.001F, "dot start");
    requireVecNear(segments[1].end, {14.0F, 0.0F}, 0.001F, "dot end");
    requireVecNear(segments[2].start, {18.0F, 0.0F}, 0.001F, "second dash start");
    requireVecNear(segments[2].end, {26.0F, 0.0F}, 0.001F, "second dash end");
}

void testPolylinePatternContinuity() {
    const std::vector<reg::overlay::Vec2f> points{
        {0.0F, 0.0F},
        {10.0F, 0.0F},
        {20.0F, 0.0F},
    };

    const auto segments = reg::overlay::tessellatePatternedPolyline(
        points,
        reg::overlay::StrokePattern{
            .pattern = reg::overlay::LinePattern::Dashed,
            .dashLength = 8.0F,
            .gapLength = 4.0F,
        });

    require(segments.size() == 2, "polyline dash continuity count");
    requireVecNear(segments[0].start, {0.0F, 0.0F}, 0.001F, "poly dash 0 start");
    requireVecNear(segments[0].end, {8.0F, 0.0F}, 0.001F, "poly dash 0 end");
    requireVecNear(segments[1].start, {12.0F, 0.0F}, 0.001F, "poly dash 1 start");
    requireVecNear(segments[1].end, {20.0F, 0.0F}, 0.001F, "poly dash 1 end");
}

reg::metadata::FrameMetadata oneTargetMetadata(
    std::uint64_t frameId,
    float x,
    float y) {
    return reg::metadata::FrameMetadata{
        .key = reg::media::FrameKey{7, frameId},
        .packetSequence = static_cast<std::uint32_t>(frameId),
        .targets = {
            reg::metadata::TargetMetadata{
                .id = 123,
                .classId = 9,
                .confidence = 0.9F,
                .bbox = {
                    .x = x,
                    .y = y,
                    .width = 0.10F,
                    .height = 0.20F,
                },
            },
        },
    };
}

void testTrackHistory() {
    using namespace std::chrono_literals;

    reg::overlay::TrackHistory history(1s);

    const auto t0 = std::chrono::steady_clock::time_point{10s};
    history.observe(oneTargetMetadata(1, 0.10F, 0.20F), t0);
    history.observe(oneTargetMetadata(2, 0.20F, 0.30F), t0 + 800ms);

    auto points = history.history(123);
    require(points.size() == 2, "track history point count");

    requireVecNear(
        points[0].normalizedCenter,
        {0.15F, 0.30F},
        0.0001F,
        "track first center");

    requireVecNear(
        points[1].normalizedCenter,
        {0.25F, 0.40F},
        0.0001F,
        "track second center");

    const auto removed = history.purge(t0 + 1500ms);
    require(removed == 1, "track purge removed point count");

    points = history.history(123);
    require(points.size() == 1, "track retention count after purge");
    require(points.front().key.frameId == 2, "track retained wrong frame");

    history.purge(t0 + 2s);
    require(history.trackCount() == 0, "expired track was not removed");
}

void testTrackHistorySeparatesEpochsAndRejectsBackwardsFrames() {
    using namespace std::chrono_literals;

    reg::overlay::TrackHistory history(5s);
    const auto t0 = std::chrono::steady_clock::time_point{20s};

    history.observe(oneTargetMetadata(10, 0.10F, 0.10F), t0);
    history.observe(oneTargetMetadata(11, 0.20F, 0.20F), t0 + 10ms);

    auto backwards = oneTargetMetadata(9, 0.90F, 0.90F);
    history.observe(backwards, t0 + 20ms);

    auto points = history.history(123);
    require(points.size() == 2, "backwards frame was added to trajectory");
    require(points.back().key.frameId == 11, "trajectory regressed to an older frame");

    auto nextEpoch = oneTargetMetadata(1, 0.30F, 0.40F);
    nextEpoch.key.streamEpoch = 8;
    history.observe(nextEpoch, t0 + 30ms);

    points = history.history(123);
    require(points.size() == 1, "new stream epoch did not reset target trajectory");
    require(points.front().key.streamEpoch == 8, "trajectory retained previous stream epoch");
    require(points.front().key.frameId == 1, "new epoch trajectory retained wrong frame");
}

} // namespace

int main() {
    try {
        testContainTransform();
        testCoverTransform();
        testZoomPanAndInverse();
        testBoundingBoxTransform();
        testDashedLine();
        testDashDotLine();
        testPolylinePatternContinuity();
        testTrackHistory();
        testTrackHistorySeparatesEpochsAndRejectsBackwardsFrames();

        std::cout << "reg_geometry_tests: all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "reg_geometry_tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
