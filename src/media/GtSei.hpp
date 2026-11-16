#pragma once

#include "media/FrameIdentity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct AVFrame;

namespace reg::media {

inline constexpr std::array<std::uint8_t, 16> kGtSeiUuid{
    0x62, 0x42, 0xb3, 0x58, 0xd3, 0x0d, 0x48, 0x4a,
    0x9e, 0xf7, 0x5e, 0x3f, 0x9c, 0x24, 0x1b, 0x07,
};
// Sender contract: JSON bytes only, excluding the 16-byte unregistered-SEI UUID.
inline constexpr std::size_t kMaxGtJsonBytes = 256U * 1024U;
inline constexpr std::size_t kMaxCameraImuSamples = 100U;

enum class GtStatus : std::uint8_t {
    Missing,
    Ready,
    NotReady,
    Busy,
    AmbiguousViewport,
    Unavailable,
    ParseError,
    FrameMismatch,
};

const char* gtStatusName(GtStatus status) noexcept;

struct GtObject {
    std::string id;
    bool visible{};
    // Continuous half-open [xmin,ymin,xmax,ymax] in video pixels.
    std::array<double, 4> xyxyVideo{};
    bool validBox{};
    double rangeM{};
    bool validRange{}; // Optional finite, non-negative GT range; never infer zero on absence.
};

// KGT1 v1 optional camera_imu extension. Invalid IMU never invalidates GT bbox.
// All timestamp and identity fields are lossless 64-bit integers.
enum class CameraImuStatus : std::uint8_t {
    Missing,
    Null,
    Ready,
    Invalid,
    FrameMismatch,
};

struct CameraImuSample {
    std::uint64_t timestampNs{};
    std::uint64_t intervalStartNs{};
    std::uint64_t sequence{};
    std::string sensorId;
    std::array<double, 3> gyroRadS{};
    std::array<double, 3> accelMS2{};
    std::uint64_t statusFlags{};
};

struct CameraImuCameraGt {
    std::optional<std::array<double, 3>> positionWorldM;
    std::optional<std::array<double, 4>> orientationWxyz;
    std::optional<std::array<double, 3>> velocityWorldMS;
    std::optional<std::array<double, 3>> accelerationWorldMS2;
    std::optional<std::array<double, 3>> gravityWorldMS2;
};

struct CameraImuMetadata {
    CameraImuStatus status{CameraImuStatus::Missing};
    std::uint64_t version{};
    std::string runId;
    std::uint64_t simulationEpoch{};
    std::uint64_t timestampNs{};
    std::string state;
    std::string sourceStatus;
    std::string axes; // camera convention (right_down_forward)
    std::optional<std::array<double, 9>> K; // row-major 3x3
    std::optional<std::array<double, 16>> tCameraImu; // row-major 4x4
    std::optional<std::array<double, 3>> extrinsicTranslationM;
    std::optional<std::array<double, 4>> extrinsicQuaternionWxyz;
    std::optional<std::uint64_t> exposureTimestampNs;
    std::optional<std::uint64_t> exposureDurationNs;
    std::vector<CameraImuSample> imu;
    CameraImuCameraGt cameraGt;
};

struct GtMetadata {
    GtStatus status{GtStatus::Missing};
    SourceFrameIdentity source{};
    std::uint32_t videoWidth{};
    std::uint32_t videoHeight{};
    std::vector<GtObject> objects;
    CameraImuMetadata cameraImu{};
    std::size_t jsonBytes{};
    std::uint64_t parseDurationUs{};
};

// Does not require FFmpeg and never throws, including on malformed input.
// JSON integer fields are parsed directly to uint64_t, never via double.
GtMetadata parseGtJson(std::span<const std::uint8_t> json) noexcept;

// Iterates every AV_FRAME_DATA_SEI_UNREGISTERED side-data entry, ignores
// unrelated UUIDs, and rejects duplicate GT UUIDs. No packet-order pairing.
GtMetadata extractGtSei(const AVFrame* frame) noexcept;

// The frame's own RGF1 identity is the sole authority for GT association.
// A mismatch, missing identity, or unexpected video dimensions disables OSD.
void validateGtForFrame(
    GtMetadata& gt,
    const std::optional<SourceFrameIdentity>& rgf1,
    int decodedWidth,
    int decodedHeight) noexcept;

} // namespace reg::media
