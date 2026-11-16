#include "media/GtSei.hpp"
#include "media/GtAccessUnit.hpp"
#include "media/CameraImuDedup.hpp"
#include "render/GtOverlayBuilder.hpp"
#include "vulkan/VideoUvRegion.hpp"
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
#include <chrono>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

std::string readyJson() {
    return R"({"type":"KGT1","version":1,"stream_epoch":18446744073709551614,"frame_id":9007199254740993,"source_time_ns":18446744073709551615,"status":"ready","video_size":[1920,1080],"surface_size":[2112,1188],"objects":[{"id":"npc-01","visible":true,"pixels":1000,"xyxy_surface":[973,578,1139,604],"xyxy_video":[884.5454545454545,525.4545454545455,1035.4545454545455,549.0909090909091],"range_m":1234.5},{"id":"npc-02","visible":false,"pixels":0,"xyxy_video":null},{"id":"npc-03","visible":true,"xyxy_video":[960.5,500.25,1100.75,575.125]}]})";
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
    check(gt.objects[0].validRange && gt.objects[0].rangeM == 1234.5,
          "valid range_m was not parsed");
    check(!gt.objects[1].validRange && !gt.objects[2].validRange,
          "missing range was invented");
    auto nullRange = readyJson();
    const auto rangeStart = nullRange.find("\"range_m\":1234.5");
    check(rangeStart != std::string::npos, "range fixture");
    nullRange.replace(rangeStart, std::string("\"range_m\":1234.5").size(),
                      "\"range_m\":null");
    check(!parse(nullRange).objects[0].validRange,
          "null range must not turn into zero");
    auto negativeRange = readyJson();
    const auto negativeStart = negativeRange.find("\"range_m\":1234.5");
    negativeRange.replace(negativeStart, std::string("\"range_m\":1234.5").size(),
                          "\"range_m\":-1");
    check(!parse(negativeRange).objects[0].validRange,
          "negative range must be rejected");
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
    check(parse(std::string(reg::media::kMaxGtJsonBytes + 1U, 'x')).status ==
          reg::media::GtStatus::ParseError, "256 KiB bound");
    check(parse(R"({"type":"KGT1","version":1,"status":"not_ready","objects":null})").status ==
          reg::media::GtStatus::NotReady, "optional fields for not_ready");
}


std::string cameraImuJson(std::size_t count = 100U, bool broken = false) {
    const std::uint64_t end = 1000000000000ULL;
    std::string records;
    for (std::size_t i = 0; i < count; ++i) {
        const auto t = end - static_cast<std::uint64_t>(count - 1U - i) * 2500000ULL;
        if (!records.empty()) records += ',';
        records += "{\"timestamp_ns\":" + std::to_string(t) +
            ",\"interval_start_ns\":" + std::to_string(t - 2500000ULL) +
            ",\"sequence\":" + std::to_string(9007199254740993ULL + i) +
            ",\"sensor_id\":\"ideal\",\"gyro_rad_s\":[0,0.1,0.2],"
            "\"accel_m_s2\":[1.0,2.0,3.0],\"status_flags\":0}";
    }
    if (broken && count > 0U) {
        const auto offset = records.find("\"gyro_rad_s\":[0,0.1,0.2]");
        check(offset != std::string::npos, "IMU fixture");
        records.replace(offset, sizeof("\"gyro_rad_s\":[0,0.1,0.2]") - 1U,
                        "\"gyro_rad_s\":[0,1e999,0]");
    }
    const std::string camera =
        "{\"version\":1,\"run_id\":\"run-1\","
        "\"simulation_epoch\":18446744073709551614,"
        "\"timestamp_ns\":1000000000000,"
        "\"state\":\"RUNNING\",\"status\":\"ready\","
        "\"axes\":\"right_down_forward\","
        "\"K\":[[1000,0,960],[0,1000,540],[0,0,1]],"
        "\"T_camera_imu\":{\"translation_m\":[0,0,0],"
        "\"quaternion_wxyz\":[1,0,0,0]},"
        "\"exposure\":{\"timestamp_ns\":1000000000000,"
        "\"duration_ns\":0},"
        "\"camera_gt\":{\"position_world_m\":[0,0,100],"
        "\"orientation_wxyz\":[1,0,0,0],"
        "\"velocity_world_m_s\":[5,0,0],"
        "\"acceleration_world_m_s2\":[0,0,0],"
        "\"gravity_world_m_s2\":[0,0,-9.81]},"
        "\"imu\":[" + records + "]}";
    auto json = readyJson();
    json.insert(json.size() - 1U, ",\"camera_imu\":" + camera);
    if (json.size() < 23790U) {
        json.insert(json.size() - 1U,
                    ",\"padding\":\"" + std::string(23790U - json.size() - 15U, 'p') + "\"");
    }
    return json;
}

void testCameraImuCompatibility() {
    using reg::media::CameraImuStatus;
    const auto old = parse(readyJson());
    check(old.status == reg::media::GtStatus::Ready &&
          old.cameraImu.status == CameraImuStatus::Missing, "legacy KGT1");
    auto withNull = readyJson();
    withNull.insert(withNull.size() - 1U, ",\"camera_imu\":null");
    check(parse(withNull).cameraImu.status == CameraImuStatus::Null,
          "null IMU is not an error");

    const auto json = cameraImuJson();
    check(json.size() > 16384U && json.size() < reg::media::kMaxGtJsonBytes,
          "live-size regression fixture");
    const auto t0 = std::chrono::steady_clock::now();
    const auto gt = parse(json);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    check(gt.status == reg::media::GtStatus::Ready &&
          gt.objects.size() == 3U, "GT bbox survives large KGT1");
    check(gt.jsonBytes == json.size() &&
          gt.cameraImu.status == CameraImuStatus::Ready, "typed IMU payload");
    const auto& imu = gt.cameraImu;
    check(imu.imu.size() == 100U && imu.K && imu.K->at(0) == 1000.0 &&
          imu.extrinsicQuaternionWxyz && imu.cameraGt.gravityWorldMS2,
          "calibration and world GT");
    check(imu.simulationEpoch == UINT64_MAX - 1U &&
          imu.imu.front().sequence == 9007199254740993ULL,
          "IMU identity exceeds double precision");
    check(imu.imu.front().gyroRadS[1] == 0.1, "finite gyro sample");
    reg::media::CameraImuDedup dedup;
    std::uint64_t delivered = 0;
    dedup.observe(imu, [&](const auto&) { ++delivered; });
    dedup.observe(imu, [&](const auto&) { ++delivered; });
    check(delivered == 100U && dedup.stats().duplicates == 100U,
          "run_id+sequence dedup overlapping windows");
    auto collision = imu;
    collision.imu.resize(1U);
    collision.imu[0].gyroRadS[0] += 0.5;
    dedup.observe(collision,[&](const auto&){++delivered;});
    check(delivered==100U && dedup.stats().conflicts==1U,
          "same sensor+sequence conflicting sample must be rejected");
    auto otherSensor=imu;
    otherSensor.imu.resize(1U);
    otherSensor.imu[0].sensorId="second-sensor";
    dedup.observe(otherSensor,[&](const auto&){++delivered;});
    check(delivered==101U,
          "same sequence from distinct sensor must not be deduplicated");

    auto restarted = imu;
    restarted.runId = "run-2";
    dedup.observe(restarted, [&](const auto&) { ++delivered; });
    check(delivered == 201U, "new run accepts reused sequences");

    const auto malformed = parse(cameraImuJson(100, true));
    check(malformed.status == reg::media::GtStatus::Ready &&
          malformed.objects.size() == 3U &&
          malformed.cameraImu.status == CameraImuStatus::Invalid,
          "semantic IMU failure never removes valid bbox");

    const auto tooMany = parse(cameraImuJson(101));
    check(tooMany.status == reg::media::GtStatus::Ready &&
          tooMany.cameraImu.status == CameraImuStatus::Invalid,
          "sample count bound without bbox damage");

    auto badKey = cameraImuJson(1);
    const auto position = badKey.find("\"simulation_epoch\":18446744073709551614");
    check(position != std::string::npos, "epoch fixture");
    badKey.insert(position, "\"frame_id\":1,");
    check(parse(badKey).cameraImu.status == CameraImuStatus::Invalid,
          "optional embedded FrameKey verified");

    // Valid JSON at the exact advertised sender bound must not be rejected.
    auto atLimit = readyJson();
    const std::string prefix = ",\"padding\":\"";
    atLimit.insert(atLimit.size() - 1U, prefix + std::string(
        reg::media::kMaxGtJsonBytes - atLimit.size() - prefix.size() - 1U, 'x') + "\"");
    check(atLimit.size() == reg::media::kMaxGtJsonBytes, "exact cap fixture");
    check(parse(atLimit).status == reg::media::GtStatus::Ready, "exact 256 KiB");
    atLimit.insert(atLimit.size() - 1U, " ");
    check(parse(atLimit).status == reg::media::GtStatus::ParseError,
          "one byte above cap");

    std::cout << "camera_imu parse " << json.size() << " bytes, " << elapsed
              << " us (synthetic, not real Reg FPS)\n";
}


std::string partialActualKgt1() {
    // Mirrors actual simulator partial-bbox-ready-imu.kgt1.bin field names.
    return R"({"type":"KGT1","version":1,"stream_epoch":8634538654274155750,"frame_id":363,"source_time_ns":1794840766195175171,"status":"not_ready","camera_imu":{"version":1,"run_id":"sky-480x270-wait0-p1-iarv05ok","simulation_epoch":1794828707111475078,"timestamp_ns":11874142500000,"status":"ready","state":"running","axes":"right_down_forward","quaternion_order":"wxyz","imu_rate_hz":400,"imu_gap":false,"imu_model":"ideal_interval_average_end_frame","shutter":"instantaneous","exposure_s":0,"K":[[935.3,0,960],[0,935.3,540],[0,0,1]],"T_camera_imu":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],"projection_row_major":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"camera_gt":{"position_world_m":[1,2,3],"rotation_optical_to_world_wxyz":[1,0,0,0],"velocity_world_m_s":[0,0,0],"acceleration_world_m_s2":[0,0,0],"gravity_world_m_s2":[0,0,-9.81]},"imu":[]}})";
}

std::vector<std::uint8_t> mapBinary(std::string_view magic,
                                     std::string_view header,
                                     std::size_t payloadSize) {
    std::vector<std::uint8_t> data(magic.begin(),magic.end());
    const auto size=static_cast<std::uint32_t>(header.size());
    for(unsigned shift:{0U,8U,16U,24U})
        data.push_back(static_cast<std::uint8_t>((size>>shift)&0xffU));
    data.insert(data.end(),header.begin(),header.end());
    data.resize(data.size()+payloadSize,0U);
    return data;
}
void testActualProducerFieldSemantics() {
    using reg::media::CameraImuStatus;
    using reg::media::GtMapKind;
    using reg::media::GtMapStatus;
    auto partial=parse(partialActualKgt1());
    check(partial.status==reg::media::GtStatus::NotReady &&
          partial.sourceValid && partial.source.key.frameId==363U,
          "non-ready bbox identity retained");
    check(partial.cameraImu.status==CameraImuStatus::Ready &&
          partial.cameraImu.imu.empty() && partial.cameraImu.imuRateHz==400U &&
          partial.cameraImu.projectionRowMajor &&
          partial.cameraImu.cameraGt.rotationOpticalToWorldWxyz,
          "camera IMU independently ready; actual projection/orientation names");
    const reg::media::SourceFrameIdentity id{
        .key={8634538654274155750ULL,363U},
        .sourceTimeNs=1794840766195175171ULL};
    reg::media::validateGtForFrame(partial,id,1920,1080);
    check(partial.status==reg::media::GtStatus::NotReady &&
          partial.cameraImu.status==CameraImuStatus::Ready,
          "independent non-ready bbox stays valid after identity join");
    auto mismatch=partial;
    auto other=id; ++other.key.frameId;
    reg::media::validateGtForFrame(mismatch,other,1920,1080);
    check(mismatch.status==reg::media::GtStatus::FrameMismatch &&
          mismatch.cameraImu.status==CameraImuStatus::FrameMismatch,
          "wrong frame never receives IMU");

    const auto skyHeader=R"({"version":1,"stream_epoch":8634538654274155750,"frame_id":363,"source_time_ns":1794840766195175171,"status":"ready","size":[32,16],"source_size":[64,32],"codec":"bitpack","bit_order":"lsb0_row_major","validity_plane":false,"unknown_pixels":0,"raw_bytes":64,"policy":1,"checksum_fnv1a32":0,"ready_ns":123})";
    auto sky=reg::media::parseGtMapBinary(mapBinary("KSM1",skyHeader,64),GtMapKind::Sky);
    check(sky.status==GtMapStatus::Encoded && sky.sourceValid &&
          sky.width==32U && sky.height==16U && sky.encoded.size()==64U,
          "sky compressed header accepted");
    reg::media::validateGtMapForFrame(sky,id);
    check(sky.status==GtMapStatus::Encoded,"sky exact identity");
    auto badSky=sky;
    reg::media::validateGtMapForFrame(badSky,other);
    check(badSky.status==GtMapStatus::FrameMismatch &&
          badSky.encoded.empty(),"sky never borrows neighboring frame");

    const auto depthHeader=R"({"version":1,"stream_epoch":8634538654274155750,"frame_id":363,"source_time_ns":1794840766195175171,"status":"ready","size":[32,16],"source_size":[64,32],"quantity":"optical_z","units":"m","dtype":"float32_le","invalid_value":0,"sky":"invalid","sampling":"integer_block_center","codec":"raw","raw_bytes":2048,"checksum_crc32":0,"valid_pixels":0,"minimum_m":0,"maximum_m":0,"projection_row_major":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]})";
    auto depth=reg::media::parseGtMapBinary(mapBinary("KDM1",depthHeader,2048),
                                              GtMapKind::Depth);
    check(depth.status==GtMapStatus::Encoded && depth.encoded.size()==2048U,
          "metric depth envelope accepted");
    auto truncated=reg::media::parseGtMapBinary(
        mapBinary("KDM1",depthHeader,2047),GtMapKind::Depth);
    check(truncated.status==GtMapStatus::Invalid,
          "truncated uncompressed depth rejected");
    auto notReady=reg::media::parseGtMapBinary(mapBinary(
        "KDM1",
        R"({"version":1,"stream_epoch":8634538654274155750,"frame_id":363,"source_time_ns":1794840766195175171,"status":"not_ready"})",
        0),GtMapKind::Depth);
    check(notReady.status==GtMapStatus::NotReady,
          "depth not-ready does not imply transport loss");

    reg::media::GtPtsAssociation association;
    auto packet=std::make_shared<reg::media::FrameGtParts>();
    packet->source=id;
    auto another=std::make_shared<reg::media::FrameGtParts>();
    auto id2=id;id2.key.frameId=364U;
    another->source=id2;
    // Simulated B-frame order: decode input PTS 10,30,20; present 10,20,30.
    association.remember(10,packet,1024);
    association.remember(30,another,2048);
    association.remember(20,packet,1024);
    check(association.take(10)->source->key.frameId==363U &&
          association.take(20)->source->key.frameId==363U &&
          association.take(30)->source->key.frameId==364U,
          "exact PTS association across reorder");
    association.remember(40,packet,1024);
    association.reset();
    check(!association.take(40) && association.pendingBytes()==0U,
          "reconnect invalidates pending GT");
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
    const auto withRange = builder.build(gt,1200U,1200U,false,true);
    check(withRange.primitives.size()==5U,
          "visible first object range label without ID");
    check(std::holds_alternative<reg::render::TextPrimitive>(withRange.primitives[2]),
          "range label is text primitive");
    const auto& rangeLabel=std::get<reg::render::TextPrimitive>(withRange.primitives[2]);
    check(rangeLabel.utf8=="1234.5 m",
          "range_m Euclidean root metres never optical Z");
    const auto withIdAndRange = builder.build(gt,1200U,1200U,true,true);
    check(std::get<reg::render::TextPrimitive>(
          withIdAndRange.primitives[2]).utf8.find("npc-01")!=std::string::npos,
          "bbox ID and range can be displayed together");
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

void testVisibleVideoSampling() {
    using reg::vulkan::computeVideoUvRegion;

    // H.264 1080p decodes into a 1088-line Vulkan image on typical encoders.
    // GT pixel coordinates address the visible 1080 lines, never the padding.
    const auto sample = computeVideoUvRegion(1920, 1080, 1920, 1088);
    check(sample.visibleWidth == 1920U && sample.visibleHeight == 1080U,
          "visible dimensions preserve GT coordinate system");
    check(sample.uvRect[0] == 0.0F && sample.uvRect[1] == 0.0F &&
          sample.uvRect[2] == 1.0F, "uncropped width/offset");
    check(std::fabs(sample.uvRect[3] - 1080.0F / 1088.0F) < 1e-7F,
          "bottom padding rows must be excluded");
    // The old full-texture sampling displayed visible y=1080 at y=1072,
    // displacing GT bboxes by almost eight output pixels at the bottom.
    const float sourceY = 1079.0F;
    const float sourceUv = sourceY / 1088.0F;
    const float displayedY =
        (sourceUv - sample.uvRect[1]) /
        (sample.uvRect[3] - sample.uvRect[1]) * 1080.0F;
    check(std::fabs(displayedY - sourceY) < 0.001F,
          "GT bbox y and displayed image y must agree");
    check(std::fabs(sourceUv * 1080.0F - sourceY) > 7.0F,
          "regression demonstrates prior eight-line displacement");

    const auto unpadded = computeVideoUvRegion(1280, 720, 1280, 720);
    check(unpadded.uvRect[3] == 1.0F && unpadded.visibleHeight == 720U,
          "no crop for full-extent surfaces");
    // FFmpeg keeps left/top crop offsets for hardware frames while already
    // subtracting right/bottom cropping from decoded width/height.
    const auto crop = computeVideoUvRegion(1920, 1080, 1920, 1088, 4, 2);
    check(crop.visibleWidth == 1916U && crop.visibleHeight == 1078U,
          "left/top cropping adjusts viewport dimensions");
    check(std::fabs(crop.uvRect[0] - 4.0F / 1920.0F) < 1e-7F &&
          std::fabs(crop.uvRect[1] - 2.0F / 1088.0F) < 1e-7F,
          "left/top cropping offsets UV origin");
    bool rejected = false;
    try {
        static_cast<void>(computeVideoUvRegion(1920, 1080, 1920, 1072));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "reject decoded dimensions outside backing VkImage");
}

int main() {
    try {
        testRemoteTextHotkeyGate();
        testVisibleVideoSampling();
        testPayloads();
        testCameraImuCompatibility();
        testActualProducerFieldSemantics();
        testFrames();
        testGeometry();
        std::cout << "gt_sei_osd_tests: PASS\n";
    } catch (const std::exception& e) {
        std::cerr << "gt_sei_osd_tests: FAIL " << e.what() << '\n';
        return 1;
    }
}
