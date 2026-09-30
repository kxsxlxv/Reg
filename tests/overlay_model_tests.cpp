#include "metadata/FrameMetadata.hpp"
#include "metadata/TrackHistory.hpp"
#include "render/OverlayPrimitives.hpp"
#include "render/TargetOverlayBuilder.hpp"
#include "video/VideoTransform.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool near(float a, float b, float epsilon = 0.01F) {
    return std::fabs(a - b) <= epsilon;
}

reg::metadata::FrameMetadata makeMetadata() {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {.streamEpoch = 2, .frameId = 100};
    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = 7,
        .classId = 3,
        .flags = 0,
        .confidence = 0.92F,
        .bbox = {
            .x = 0.25F,
            .y = 0.25F,
            .width = 0.50F,
            .height = 0.50F,
        },
    });
    return metadata;
}

void aspectFitMapsNormalizedCoordinates() {
    reg::video::VideoTransform transform(
        1920,
        1080,
        1280,
        1024);

    const auto viewport = transform.viewport();

    require(near(viewport.width, 1280.0F), "unexpected viewport width");
    require(near(viewport.height, 720.0F), "unexpected viewport height");
    require(near(viewport.x, 0.0F), "unexpected viewport x");
    require(near(viewport.y, 152.0F), "unexpected vertical letterbox");

    const auto center = transform.normalizedToScreen(reg::video::Vec2{0.5F, 0.5F});
    require(near(center.x, 640.0F), "normalized center x mismatch");
    require(near(center.y, 512.0F), "normalized center y mismatch");

    const auto rect = transform.normalizedToScreen(
        reg::video::RectF{0.25F, 0.25F, 0.5F, 0.5F});
    require(near(rect.x, 320.0F), "mapped rect x mismatch");
    require(near(rect.y, 332.0F), "mapped rect y mismatch");
    require(near(rect.width, 640.0F), "mapped rect width mismatch");
    require(near(rect.height, 360.0F), "mapped rect height mismatch");
}

void zoomAndPanUseOneCanonicalTransform() {
    reg::video::VideoTransform transform(
        1920,
        1080,
        1920,
        1080);

    transform.setZoom(2.0F);

    auto center = transform.normalizedToScreen(reg::video::Vec2{0.5F, 0.5F});
    require(near(center.x, 960.0F), "zoom changed source center x");
    require(near(center.y, 540.0F), "zoom changed source center y");

    transform.setPan({0.1F, 0.0F});
    center = transform.normalizedToScreen(reg::video::Vec2{0.6F, 0.5F});
    require(near(center.x, 960.0F), "pan center mapping mismatch");
}

void trackHistoryMaintainsBoundedRecentPoints() {
    reg::metadata::TrackHistory history(100ms, 3);
    auto metadata = makeMetadata();

    const auto t0 = std::chrono::steady_clock::time_point{1s};

    history.update(metadata, t0);
    metadata.targets[0].bbox.x += 0.01F;
    history.update(metadata, t0 + 10ms);
    metadata.targets[0].bbox.x += 0.01F;
    history.update(metadata, t0 + 20ms);
    metadata.targets[0].bbox.x += 0.01F;
    history.update(metadata, t0 + 30ms);

    auto points = history.points(7);
    require(points.size() == 3, "track did not enforce point capacity");

    history.update(metadata, t0 + 200ms);
    points = history.points(7);
    require(points.size() == 1, "track did not prune expired points");
}

void sceneBuilderCreatesExpectedTargetPrimitives() {
    auto metadata = makeMetadata();
    reg::metadata::TrackHistory history(5s, 32);

    const auto t0 = std::chrono::steady_clock::time_point{1s};
    history.update(metadata, t0);

    metadata.targets[0].bbox.x += 0.01F;
    history.update(metadata, t0 + 16ms);

    reg::video::VideoTransform transform(
        1920,
        1080,
        1920,
        1080);

    reg::render::TargetOverlayBuilder builder;
    builder.setMasterAlpha(0.5F);

    const auto scene =
        builder.build(metadata, history, transform);

    require(scene.primitives.size() == 5, "unexpected primitive count");

    require(
        std::holds_alternative<reg::render::FilledRectPrimitive>(
            scene.primitives[0]),
        "primitive 0 must be bbox fill");
    require(
        std::holds_alternative<reg::render::RectPrimitive>(
            scene.primitives[1]),
        "primitive 1 must be bbox stroke");
    require(
        std::holds_alternative<reg::render::CrosshairPrimitive>(
            scene.primitives[2]),
        "primitive 2 must be crosshair");
    require(
        std::holds_alternative<reg::render::PolylinePrimitive>(
            scene.primitives[3]),
        "primitive 3 must be trajectory");
    require(
        std::holds_alternative<reg::render::TextPrimitive>(
            scene.primitives[4]),
        "primitive 4 must be label");

    const auto& fill =
        std::get<reg::render::FilledRectPrimitive>(
            scene.primitives[0]);
    require(near(fill.color.a, 0.05F), "master alpha not applied to fill");

    const auto& label =
        std::get<reg::render::TextPrimitive>(
            scene.primitives[4]);
    require(
        label.utf8.find("ID 7") != std::string::npos,
        "label does not include target ID");
    require(
        label.utf8.find("C3") != std::string::npos,
        "label does not include class ID");
}

void provenanceFlagsDriveOverlayStyle() {
    reg::metadata::TrackHistory history(5s, 32);
    reg::video::VideoTransform transform(
        1920,
        1080,
        1920,
        1080);
    reg::render::TargetOverlayBuilder builder;

    auto detectorMetadata = makeMetadata();
    detectorMetadata.targets[0].flags = 0x0001U;
    const auto detectorScene =
        builder.build(detectorMetadata, history, transform);

    require(detectorScene.primitives.size() == 4,
        "detector scene must contain fill/stroke/crosshair/label");
    const auto& detectorRect =
        std::get<reg::render::RectPrimitive>(detectorScene.primitives[1]);
    const auto& detectorLabel =
        std::get<reg::render::TextPrimitive>(detectorScene.primitives[3]);
    require(
        detectorRect.style.pattern == reg::render::LinePattern::Solid,
        "YOLO bbox must be solid");
    require(
        detectorLabel.utf8.find("YOLO") != std::string::npos,
        "YOLO label provenance missing");

    auto propagatedMetadata = makeMetadata();
    propagatedMetadata.targets[0].flags = 0x0002U;
    const auto propagatedScene =
        builder.build(propagatedMetadata, history, transform);

    require(propagatedScene.primitives.size() == 4,
        "propagated scene must contain fill/stroke/crosshair/label");
    const auto& propagatedRect =
        std::get<reg::render::RectPrimitive>(propagatedScene.primitives[1]);
    const auto& propagatedLabel =
        std::get<reg::render::TextPrimitive>(propagatedScene.primitives[3]);
    require(
        propagatedRect.style.pattern == reg::render::LinePattern::Dashed,
        "OFA bbox must be dashed");
    require(
        propagatedLabel.utf8.find("OFA") != std::string::npos,
        "OFA label provenance missing");
}

} // namespace

int main() {
    try {
        aspectFitMapsNormalizedCoordinates();
        zoomAndPanUseOneCanonicalTransform();
        trackHistoryMaintainsBoundedRecentPoints();
        sceneBuilderCreatesExpectedTargetPrimitives();
        provenanceFlagsDriveOverlayStyle();

        std::cout << "overlay_model_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "overlay_model_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
