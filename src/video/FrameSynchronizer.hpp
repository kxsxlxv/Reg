#pragma once

#include "metadata/MetadataStore.hpp"
#include "video/OverlayFrameBuffer.hpp"

#include <cstddef>
#include <memory>
#include <utility>

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
        std::size_t droppedFrames{};
    };

    FrameSynchronizer(
        Buffer& video,
        metadata::MetadataStore& metadata)
        : video_(video),
          metadata_(metadata) {}

    typename Buffer::PushResult pushFrame(
        media::FrameKey key,
        PayloadPtr frame,
        TimePoint receivedAt) {
        auto result =
            video_.push(key, std::move(frame), receivedAt);

        if (result.evictedKey) {
            metadata_.discardThrough(*result.evictedKey);
        }

        return result;
    }

    Result next(TimePoint now) {
        Result result{};
        std::size_t dropped = 0;

        while (true) {
            auto entry = video_.popReady(now);
            if (!entry) {
                if (dropped != 0) {
                    result.action = SyncAction::Drop;
                    result.droppedFrames = dropped;
                }
                return result;
            }

            auto frameMetadata = metadata_.take(entry->key);
            metadata_.discardThrough(entry->key);

            if (!frameMetadata) {
                ++dropped;
                result.key = entry->key;
                result.frame = std::move(entry->payload);
                continue;
            }

            return Result{
                .action = SyncAction::Present,
                .key = entry->key,
                .frame = std::move(entry->payload),
                .metadata = std::move(frameMetadata),
                .droppedFrames = dropped,
            };
        }
    }

private:
    Buffer& video_;
    metadata::MetadataStore& metadata_;
};

} // namespace reg::video
