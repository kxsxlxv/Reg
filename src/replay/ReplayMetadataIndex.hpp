#pragma once

#include "media/FrameIdentity.hpp"
#include "metadata/FrameMetadata.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <unordered_map>

namespace reg::replay {

struct ReplayMetadataIndexStats {
    std::size_t filesLoaded{};
    std::size_t recordsLoaded{};
    std::size_t duplicateFrameKeys{};
};

class ReplayMetadataIndex final {
public:
    static ReplayMetadataIndex loadDirectory(
        const std::filesystem::path& directory);

    metadata::FrameMetadataPtr find(
        media::FrameKey key) const;

    const ReplayMetadataIndexStats& stats() const noexcept {
        return stats_;
    }

    std::size_t size() const noexcept {
        return metadata_.size();
    }

private:
    std::unordered_map<
        media::FrameKey,
        metadata::FrameMetadataPtr,
        media::FrameKeyHash>
        metadata_;

    ReplayMetadataIndexStats stats_{};
};

} // namespace reg::replay
