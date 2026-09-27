#pragma once

#include "metadata/MetadataStore.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace reg::video {

enum class SyncAction {
    Wait,
    Present,
    Drop,
};

template <typename Payload>
class FrameSynchronizer final {
public:
    using Buffer = OverlayFrameBuffer<Payload>;
    using PayloadPtr = typename Buffer::PayloadPtr;
    using TimePoint = typename Buffer::TimePoint;

    struct Result {
        SyncAction action{SyncAction::Wait};
        media::FrameKey key{};
        PayloadPtr frame;
        metadata::FrameMetadataPtr metadata;
    };

    FrameSynchronizer(
        Buffer& video,
        metadata::MetadataStore& metadata)
        : video_(video),
          metadata_(metadata) {}

    Result next(TimePoint now) {
        const typename Buffer::Entry* front = video_.front();
        if (front == nullptr) {
            return {};
        }

        if (now < front->deadline) {
            return Result{
                .action = SyncAction::Wait,
                .key = front->key,
            };
        }

        auto entry = video_.popFront();
        auto frameMetadata = metadata_.take(entry.key);

        if (!frameMetadata) {
            return Result{
                .action = SyncAction::Drop,
                .key = entry.key,
                .frame = std::move(entry.payload),
            };
        }

        return Result{
            .action = SyncAction::Present,
            .key = entry.key,
            .frame = std::move(entry.payload),
            .metadata = std::move(frameMetadata),
        };
    }

private:
    Buffer& video_;
    metadata::MetadataStore& metadata_;
};

} // namespace reg::video
