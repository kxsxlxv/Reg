#include "diagnostics/GtIngestStats.hpp"
#include "media/CameraImuDedup.hpp"

#include <cstdint>
#include <stdexcept>

static void check(bool value) {
    if (!value) throw std::runtime_error("KGT1 ingest stats regression");
}
int main() {
    reg::diagnostics::GtIngestStats stats;
    reg::media::GtMetadata missing;
    stats.observe(missing);
    auto result = stats.snapshot();
    check(result.framesWithKgt1 == 0U);

    reg::media::GtMetadata ready;
    ready.status = reg::media::GtStatus::Ready;
    ready.jsonBytes = 23790U;
    ready.parseDurationUs = 420U;
    ready.cameraImu.status = reg::media::CameraImuStatus::Ready;
    ready.cameraImu.runId = "run-1";
    ready.cameraImu.imu.resize(100U);
    for (std::size_t i = 0; i < 100U; ++i)
        ready.cameraImu.imu[i].sequence = static_cast<std::uint64_t>(i);
    stats.observe(ready);
    stats.observe(ready); // overlapping windows count as repeated samples

    reg::media::CameraImuDedup dedup;
    std::uint64_t newSamples = 0U;
    dedup.observe(ready.cameraImu, [&](const auto&) { ++newSamples; });
    dedup.observe(ready.cameraImu, [&](const auto&) { ++newSamples; });
    check(newSamples == 100U && dedup.stats().duplicates == 100U);

    auto invalid = ready;
    invalid.cameraImu.status = reg::media::CameraImuStatus::Invalid;
    invalid.cameraImu.imu.clear();
    stats.observe(invalid);
    reg::media::GtMetadata tooLarge;
    tooLarge.status = reg::media::GtStatus::ParseError;
    tooLarge.jsonBytes = reg::media::kMaxGtJsonBytes + 1U;
    stats.observe(tooLarge);

    reg::media::FrameGtParts frameGt;
    frameGt.sky.status=reg::media::GtMapStatus::Encoded;
    frameGt.depth.status=reg::media::GtMapStatus::NotReady;
    frameGt.identicalDuplicates=1;
    stats.observeMaps(frameGt);
    frameGt.sky.status=reg::media::GtMapStatus::FrameMismatch;
    frameGt.depth.status=reg::media::GtMapStatus::Invalid;
    frameGt.conflictingFields=2;
    frameGt.malformedAu=true;
    stats.observeMaps(frameGt);
    result = stats.snapshot();
    check(result.framesWithKgt1 == 4U && result.gtReady == 3U &&
          result.gtParseErrors == 1U && result.gtTooLarge == 1U &&
          result.imuReady == 2U && result.imuInvalid == 1U &&
          result.imuSamplesParsed == 200U &&
          result.gtParseTotalUs == 1260U && result.gtParseMaxUs == 420U &&
          result.gtJsonMaxBytes == reg::media::kMaxGtJsonBytes + 1U &&
          result.skyEncoded == 1U && result.skyInvalid == 1U &&
          result.depthNonReady == 1U && result.depthInvalid == 1U &&
          result.auIdenticalDuplicates == 1U &&
          result.auConflicts == 2U && result.auMalformed == 1U);
}
