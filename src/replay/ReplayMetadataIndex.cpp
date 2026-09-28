#include "replay/ReplayMetadataIndex.hpp"

#include "recorder/MetadataJournal.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace reg::replay {

void ReplayMetadataIndex::loadDirectory(
    const std::filesystem::path& directory) {
    clear();

    if (!std::filesystem::exists(directory)) {
        throw std::runtime_error(
            "replay metadata directory does not exist: " +
            directory.string());
    }

    std::vector<std::filesystem::path> files;

    for (const auto& entry :
         std::filesystem::directory_iterator(
             directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() ==
            ".cvmj") {
            files.push_back(entry.path());
        }
    }

    std::sort(
        files.begin(),
        files.end());

    for (const auto& path : files) {
        const auto journal =
            recorder::readMetadataJournalFile(
                path);

        for (const auto& record :
             journal.records) {
            auto value =
                std::make_shared<
                    const metadata::FrameMetadata>(
                        record.metadata);

            // First recorded metadata for a FrameKey wins. This mirrors the
            // live metadata-store duplicate policy.
            metadata_.try_emplace(
                value->key,
                std::move(value));
        }
    }
}

metadata::FrameMetadataPtr
ReplayMetadataIndex::find(
    media::FrameKey key) const {
    const auto it =
        metadata_.find(key);

    return it == metadata_.end()
        ? metadata::FrameMetadataPtr{}
        : it->second;
}

void ReplayMetadataIndex::clear() {
    metadata_.clear();
}

} // namespace reg::replay
