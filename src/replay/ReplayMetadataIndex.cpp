#include "replay/ReplayMetadataIndex.hpp"

#include "recorder/MetadataJournal.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace reg::replay {

ReplayMetadataIndex
ReplayMetadataIndex::loadDirectory(
    const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument(
            "replay metadata directory must not be empty");
    }

    if (!std::filesystem::exists(directory)) {
        throw std::runtime_error(
            "replay metadata directory does not exist: " +
            directory.string());
    }

    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error(
            "replay metadata path is not a directory: " +
            directory.string());
    }

    std::vector<std::filesystem::path> journals;

    for (const auto& entry :
         std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() == ".cvmj") {
            journals.push_back(entry.path());
        }
    }

    std::sort(journals.begin(), journals.end());

    ReplayMetadataIndex index;

    for (const auto& journalPath : journals) {
        const auto journal =
            recorder::readMetadataJournalFile(
                journalPath);

        ++index.stats_.filesLoaded;

        for (auto& record : journal.records) {
            auto metadata =
                std::make_shared<
                    const metadata::FrameMetadata>(
                        std::move(record.metadata));

            const auto [it, inserted] =
                index.metadata_.emplace(
                    metadata->key,
                    std::move(metadata));

            if (inserted) {
                ++index.stats_.recordsLoaded;
            } else {
                ++index.stats_.duplicateFrameKeys;
            }
        }
    }

    return index;
}

metadata::FrameMetadataPtr
ReplayMetadataIndex::find(
    media::FrameKey key) const {
    const auto it = metadata_.find(key);
    return it == metadata_.end()
        ? metadata::FrameMetadataPtr{}
        : it->second;
}

} // namespace reg::replay
