#pragma once

#include "metadata/FrameMetadata.hpp"

#include <cstddef>
#include <filesystem>
#include <unordered_map>

namespace reg::replay {

class ReplayMetadataIndex final {
public:
    void loadDirectory(
        const std::filesystem::path& directory);

    metadata::FrameMetadataPtr find(
        media::FrameKey key) const;

    std::size_t size() const noexcept {
        return metadata_.size();
    }

    void clear();

private:
    std::unordered_map<
        media::FrameKey,
        metadata::FrameMetadataPtr,
        media::FrameKeyHash>
        metadata_;
};

} // namespace reg::replay
