#include "media/FrameIdentity.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace reg::media {
namespace {

std::optional<SourceFrameIdentity> parseSideData(
    const AVFrameSideData& sideData) {
    constexpr std::size_t uuidSize =
        kFrameIdentitySeiUuid.size();

    if (sideData.size <
        uuidSize + kFrameIdentityPayloadSize) {
        return std::nullopt;
    }

    const std::span<const std::uint8_t> data(
        sideData.data,
        sideData.size);

    if (!std::equal(
            kFrameIdentitySeiUuid.begin(),
            kFrameIdentitySeiUuid.end(),
            data.begin())) {
        return std::nullopt;
    }

    return decodeFrameIdentityPayload(
        data.subspan(uuidSize));
}

} // namespace

std::optional<SourceFrameIdentity>
extractFrameIdentity(const AVFrame* frame) {
    if (frame == nullptr) {
        return std::nullopt;
    }

    for (int i = 0; i < frame->nb_side_data; ++i) {
        const AVFrameSideData* sideData =
            frame->side_data[i];

        if (sideData != nullptr &&
            sideData->type ==
                AV_FRAME_DATA_SEI_UNREGISTERED) {
            if (auto identity =
                    parseSideData(*sideData)) {
                return identity;
            }
        }
    }

    return std::nullopt;
}

} // namespace reg::media
