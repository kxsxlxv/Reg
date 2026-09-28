#include "recorder/MetadataJournal.hpp"
#include "replay/ReplayClock.hpp"
#include "replay/ReplayMetadataIndex.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
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
    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = frameId,
            .classId = 5,
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

void replayClockPreservesMediaSpacing() {
    const auto wall =
        std::chrono::steady_clock::time_point{100s};

    reg::replay::ReplayClock clock(1.0);

    const auto first =
        clock.targetTime(
            10s,
            wall);

    const auto second =
        clock.targetTime(
            10s + 16ms,
            wall + 1s);

    require(
        first == wall,
        "first replay frame must start immediately");

    require(
        second - first == 16ms,
        "1x replay spacing mismatch");

    reg::replay::ReplayClock doubleSpeed(2.0);

    const auto fastFirst =
        doubleSpeed.targetTime(
            1s,
            wall);

    const auto fastSecond =
        doubleSpeed.targetTime(
            1s + 20ms,
            wall);

    require(
        fastSecond - fastFirst == 10ms,
        "2x replay spacing mismatch");
}

void metadataIndexUsesExactFrameKeys() {
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    const auto directory =
        std::filesystem::temp_directory_path() /
        ("reg_replay_index_test_" + unique);

    std::filesystem::remove_all(directory);

    try {
        const auto t0 =
            std::chrono::steady_clock::time_point{10s};

        reg::recorder::MetadataJournalWriter writer(
            reg::recorder::MetadataJournalConfig{
                .directory = directory,
                .targetSegmentDuration = 5s,
                .retention = 10s,
            });

        writer.write(
            makeMetadata(
                77,
                1000,
                1,
                t0));

        writer.write(
            makeMetadata(
                77,
                1001,
                2,
                t0 + 16ms));

        writer.close();

        reg::replay::ReplayMetadataIndex index;
        index.loadDirectory(directory);

        require(
            index.size() == 2,
            "replay metadata index size mismatch");

        const auto first =
            index.find({77, 1000});

        require(
            first != nullptr,
            "exact replay FrameKey was not found");

        require(
            first->targets.size() == 1 &&
                first->targets[0].id == 1000,
            "replay metadata payload mismatch");

        require(
            !index.find({77, 999}),
            "replay index matched wrong frame ID");

        require(
            !index.find({78, 1000}),
            "replay index matched wrong stream epoch");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    try {
        replayClockPreservesMediaSpacing();
        metadataIndexUsesExactFrameKeys();

        std::cout
            << "replay_foundation_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "replay_foundation_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
