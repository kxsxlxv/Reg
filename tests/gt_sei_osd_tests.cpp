#include "media/GtSei.hpp"
#include "render/GtOverlayBuilder.hpp"
#include "remote/RemoteTextFocusGate.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

std::string readyJson() {
    return R"({"type":"KGT1","version":1,"stream_epoch":18446744073709551614,"frame_id":9007199254740993,"source_time_ns":18446744073709551615,"status":"ready","video_size":[1920,1080],"surface_size":[2112,1188],"objects":[{"id":"npc-01","visible":true,"pixels":1000,"xyxy_surface":[973,578,1139,604],"xyxy_video":[884.5454545454545,525.4545454545455,1035.4545454545455,549.0909090909091]},{"id":"npc-02","visible":false,"pixels":0,"xyxy_video":null},{"id":"npc-03","visible":true,"xyxy_video":[960.5,500.25,1100.75,575.125]}]})";
}

reg::media::GtMetadata parse(std::string_view text) {
    return reg::media::parseGtJson(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void appendSei(AVFrame* frame, std::array<std::uint8_t, 16> uuid, std::string_view json) {
    AVFrameSideData* side = av_frame_new_side_data(
        frame, AV_FRAME_DATA_SEI_UNREGISTERED, uuid.size() + json.size());
    check(side != nullptr, "alloc side data");
    std::copy(uuid.begin(), uuid.end(), side->data);
    std::memcpy(side->data + uuid.size(), json.data(), json.size());
}

void testPayloads() {
    auto gt = parse(readyJson());
    check(gt.status == reg::media::GtStatus::Ready, "ready");
    check(gt.source.key.streamEpoch == UINT64_MAX - 1U, "epoch >2^53");
    check(gt.source.key.frameId == 9007199254740993ULL, "frame_id >2^53");
    check(gt.source.sourceTimeNs == UINT64_MAX, "source_time_ns UINT64_MAX");
    check(gt.objects.size() == 3U && !gt.objects[1].visible, "invisible object");
    check(std::fabs(gt.objects[0].xyxyVideo[0] - 884.5454545454545) < 1e-8,
          "fractional position");
    check(parse(R"({"type":"KGT1","version":1,"status":"not_ready"})").status ==
          reg::media::GtStatus::NotReady, "not ready");
    check(parse(R"({"type":"KGT1","version":1,"status":"busy"})").status ==
          reg::media::GtStatus::Busy, "busy");
    check(parse(R"({"type":"KGT1","version":1,"status":"ambiguous_viewport"})").status ==
          reg::media::GtStatus::AmbiguousViewport, "ambiguous");
    check(parse(R"({"type":"KGT1","version":1,"status":"unknown"})").status ==
          reg::media::GtStatus::Unavailable, "unknown status");
    check(parse(R"({"type":"KGT1","version":2,"status":"ready"})").status ==
          reg::media::GtStatus::ParseError, "unsupported version");
    check(parse("{not json").status == reg::media::GtStatus::ParseError,
          "damaged JSON");
    check(parse(R"({"type":"KGT1","type":"KGT1","version":1,"status":"ready"})").status ==
          reg::media::GtStatus::ParseError, "duplicate key");
    check(parse(R"({"type":"KGT1","version":1,"status":"ready","stream_epoch":18446744073709551616})").status ==
          reg::media::GtStatus::ParseError, "uint64 overflow");
    check(parse(std::string(16385U, 'x')).status ==
          reg::media::GtStatus::ParseError, "16 KiB bound");
    check(parse(R"({"type":"KGT1","version":1,"status":"not_ready","objects":null})").status ==
          reg::media::GtStatus::NotReady, "optional fields for not_ready");
}

void testFrames() {
    AVFrame* frame = av_frame_alloc();
    check(frame != nullptr, "alloc frame");
    const auto json = readyJson();
    appendSei(frame, std::array<std::uint8_t, 16>{}, "{}");
    const reg::media::SourceFrameIdentity reference{
        .key = {UINT64_MAX - 1U, 9007199254740993ULL},
        .sourceTimeNs = UINT64_MAX,
    };
    const auto encoded = reg::media::encodeFrameIdentityPayload(reference);
    appendSei(frame, reg::media::kFrameIdentitySeiUuid,
        std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
    appendSei(frame, reg::media::kGtSeiUuid, json);
    // No positional assumption: GT is surrounded by unrelated SEI records.
    appendSei(frame, std::array<std::uint8_t, 16>{
        0x12,0x32,0x32,0x32,0,0,0,0,0,0,0,0,0,0,0,1}, "{}");
    auto gt = reg::media::extractGtSei(frame);
    check(gt.status == reg::media::GtStatus::Ready, "multi SEI traversal");
    const auto source = reg::media::extractFrameIdentity(frame);
    check(source.has_value() && source->key == reference.key &&
          source->sourceTimeNs == reference.sourceTimeNs,
          "RGF1 survives alongside GT and unrelated SEI");
    reg::media::validateGtForFrame(gt, source, 1920, 1080);
    check(gt.status == reg::media::GtStatus::Ready, "identity association");
    auto wrongTime = reg::media::extractGtSei(frame);
    reg::media::validateGtForFrame(wrongTime, reg::media::SourceFrameIdentity{
        .key = {UINT64_MAX - 1U, 9007199254740993ULL},
        .sourceTimeNs = UINT64_MAX - 1U}, 1920, 1080);
    check(wrongTime.status == reg::media::GtStatus::FrameMismatch &&
          wrongTime.objects.empty(), "reject next-frame identity");
    auto wrongDims = reg::media::extractGtSei(frame);
    reg::media::validateGtForFrame(wrongDims, reg::media::SourceFrameIdentity{
        .key = {UINT64_MAX - 1U, 9007199254740993ULL},
        .sourceTimeNs = UINT64_MAX}, 1280, 720);
    check(wrongDims.status == reg::media::GtStatus::FrameMismatch,
          "decoded dimensions differ");
    auto missingId = reg::media::extractGtSei(frame);
    reg::media::validateGtForFrame(missingId, std::nullopt, 1920, 1080);
    check(missingId.status == reg::media::GtStatus::FrameMismatch,
          "missing RGF1 identity");
    appendSei(frame, reg::media::kGtSeiUuid, json);
    check(reg::media::extractGtSei(frame).status ==
          reg::media::GtStatus::ParseError, "duplicate GT SEI");
    av_frame_free(&frame);

    frame = av_frame_alloc();
    appendSei(frame, std::array<std::uint8_t, 16>{}, json);
    check(reg::media::extractGtSei(frame).status ==
          reg::media::GtStatus::Missing, "unknown uuid ignored");
    av_frame_free(&frame);
}

void testGeometry() {
    auto gt = parse(readyJson());
    reg::render::GtOverlayBuilder builder;
    const auto scene = builder.build(gt, 1200U, 1200U, true);
    check(scene.primitives.size() == 6U, "2 visible rects plus IDs and clip pair");
    check(std::holds_alternative<reg::render::ClipPushPrimitive>(scene.primitives.front()),
          "start clipping");
    check(std::holds_alternative<reg::render::ClipPopPrimitive>(scene.primitives.back()),
          "end clipping");
    const auto& clip = std::get<reg::render::ClipPushPrimitive>(
        scene.primitives.front()).rect;
    check(std::fabs(clip.y - 262.5F) < 0.01F &&
          std::fabs(clip.height - 675.0F) < 0.01F, "letterbox on resize");
    const auto& box = std::get<reg::render::RectPrimitive>(scene.primitives[1]).rect;
    check(std::fabs(box.x - 552.8409F) < 0.1F, "fractional X transformed");
    check(std::fabs(box.y - 590.9091F) < 0.2F, "fractional Y transformed");
    check(std::fabs(box.width - 94.318F) < 0.2F, "xyxy right endpoint");
    const auto withoutIds = builder.build(gt, 1200U, 1200U, false);
    check(withoutIds.primitives.size() == 4U, "independent ID toggle");
    check(builder.build(parse(R"({"type":"KGT1","version":1,"status":"busy"})"),
                        1200U, 1200U, true).empty(), "no stale boxes on not-ready");
    check(builder.build(gt, 0U, 1200U, true).empty(), "no viewport");
}
void testRemoteTextHotkeyGate() {
    using reg::remote::RemoteTextFocus;
    using reg::remote::gtHotkeyAllowed;
    check(gtHotkeyAllowed(RemoteTextFocus::Disconnected),
          "allow T when no remote input client");
    check(!gtHotkeyAllowed(RemoteTextFocus::Unknown),
          "legacy remote client must block T");
    check(gtHotkeyAllowed(RemoteTextFocus::NotEditing),
          "allow T when upgraded remote client is not editing");
    check(!gtHotkeyAllowed(RemoteTextFocus::Editing),
          "typing T in a remote input must not toggle GT");
}
}

int main() {
    try {
        testRemoteTextHotkeyGate();
        testPayloads();
        testFrames();
        testGeometry();
        std::cout << "gt_sei_osd_tests: PASS\n";
    } catch (const std::exception& e) {
        std::cerr << "gt_sei_osd_tests: FAIL " << e.what() << '\n';
        return 1;
    }
}
