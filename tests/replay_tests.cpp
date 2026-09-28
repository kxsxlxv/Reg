#include "metadata/FrameMetadata.hpp"
#include "recorder/MetadataJournal.hpp"
#include "replay/ReplayClock.hpp"
#include "replay/ReplayMetadataIndex.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

reg::metadata::FrameMetadata makeMetadata(
    std::uint64_t epoch,
    std::uint64_t frameId,
    std::uint32_t sequence,
    std::chrono::steady_clock::time_point receivedAt) {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {
        .streamEpoch = epoch,
        .frameId = frameId,
    };
    metadata.sequence = sequence;
    metadata.receivedAt = receivedAt;
    metadata.cvBeginNs = frameId * 1000;
    metadata.cvEndNs = metadata.cvBeginNs + 500;
    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = frameId,
            .classId = 2,
            .confidence = 0.9F,
            .bbox = {
                .x = 0.1F,
                .y = 0.2F,
                .width = 0.3F,
                .height = 0.4F,
            },
        });
    return metadata;
}

void replayClockUsesSourceTimeAndResetsOnEpochChange() {
    reg::replay::ReplayClock clock;

    const auto wall0 =
        std::chrono::steady_clock::time_point{10s};

    const reg::media::SourceFrameIdentity first{
        .key = {.streamEpoch = 7, .frameId = 100},
        .sourceTimeNs = 1'000'000'000ULL,
    };

    require(
        clock.targetTime(first, wall0) == wall0,
        "first replay frame must anchor to current wall time");

    const reg::media::SourceFrameIdentity second{
        .key = {.streamEpoch = 7, .frameId = 101},
        .sourceTimeNs = 1'016'666'667ULL,
    };

    require(
        clock.targetTime(second, wall0 + 1ms) ==
            wall0 + 16'666'667ns,
        "replay clock did not preserve source timestamp delta");

    const auto wall1 =
        wall0 + 2s;

    const reg::media::SourceFrameIdentity newEpoch{
        .key = {.streamEpoch = 8, .frameId = 1},
        .sourceTimeNs = 100,
    };

    require(
        clock.targetTime(newEpoch, wall1) == wall1,
        "epoch change must reset replay clock anchor");

    const auto wall2 =
        wall1 + 500ms;

    const reg::media::SourceFrameIdentity regressed{
        .key = {.streamEpoch = 8, .frameId = 2},
        .sourceTimeNs = 50,
    };

    require(
        clock.targetTime(regressed, wall2) == wall2,
        "source-time regression must reset replay clock anchor");
}

void metadataIndexLoadsExactFrameKeys() {
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    const auto directory =
        std::filesystem::temp_directory_path() /
        ("reg_replay_index_test_" + unique);

    std::filesystem::remove_all(directory);

    const auto t0 =
        std::chrono::steady_clock::time_point{5s};

    try {
        reg::recorder::MetadataJournalWriter writer(
            reg::recorder::MetadataJournalConfig{
                .directory = directory,
                .targetSegmentDuration = 25ms,
                .retention = 10s,
            });

        writer.write(
            makeMetadata(42, 1000, 1, t0));
        writer.write(
            makeMetadata(42, 1001, 2, t0 + 10ms));
        writer.write(
            makeMetadata(42, 1002, 3, t0 + 50ms));
        writer.close();

        const auto index =
            reg::replay::ReplayMetadataIndex::
                loadDirectory(directory);

        require(
            index.stats().filesLoaded == 2,
            "replay index should load both rotated journals");
        require(
            index.stats().recordsLoaded == 3,
            "replay index record count mismatch");
        require(
            index.stats().duplicateFrameKeys == 0,
            "unexpected duplicate FrameKey in replay index");

        for (std::uint64_t frameId = 1000;
             frameId <= 1002;
             ++frameId) {
            const auto metadata =
                index.find(
                    reg::media::FrameKey{
                        .streamEpoch = 42,
                        .frameId = frameId,
                    });

            require(
                metadata != nullptr,
                "exact replay FrameKey lookup failed");
            require(
                metadata->key.frameId == frameId,
                "replay index returned wrong frame metadata");
        }

        require(
            index.find({42, 9999}) == nullptr,
            "missing replay FrameKey must not match another frame");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    try {
        replayClockUsesSourceTimeAndResetsOnEpochChange();
        metadataIndexLoadsExactFrameKeys();

        std::cout << "replay_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "replay_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
