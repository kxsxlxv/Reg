#include "jetson/MetadataBridge.hpp"

#include <utility>

namespace reg::jetson {

MetadataBridge::MetadataBridge(
    std::string_view remoteAddress,
    std::uint16_t remotePort,
    std::uint32_t initialSequence)
    : sender_(
          remoteAddress,
          remotePort,
          initialSequence) {}

std::optional<FrameContext>
MetadataBridge::beginFrame(
    const AVFrame* decodedFrame,
    std::uint64_t cvBeginNs) {
    ++decodedFramesObserved_;

    const auto identity =
        media::extractFrameIdentity(
            decodedFrame);

    if (!identity.has_value()) {
        ++framesMissingIdentity_;
        return std::nullopt;
    }

    return FrameContext{
        *identity,
        cvBeginNs};
}

bool MetadataBridge::sendResult(
    const FrameContext& context,
    std::uint64_t cvEndNs,
    metadata::FrameMetadata metadata) {
    metadata.key =
        context.key();
    metadata.cvBeginNs =
        context.cvBeginNs();
    metadata.cvEndNs =
        cvEndNs;

    return sender_.send(
        std::move(metadata));
}

MetadataBridgeStats
MetadataBridge::stats() const noexcept {
    return MetadataBridgeStats{
        .decodedFramesObserved =
            decodedFramesObserved_,
        .framesMissingIdentity =
            framesMissingIdentity_,
        .transport =
            sender_.stats(),
    };
}

} // namespace reg::jetson
