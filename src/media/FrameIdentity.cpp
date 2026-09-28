#include "media/FrameIdentity.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace reg::media {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'R', 'G', 'F', '1'};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kPayloadSize = 32;

std::uint16_t readLe16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint64_t readLe64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::optional<SourceFrameIdentity> parseSideData(const AVFrameSideData& sideData) {
    constexpr std::size_t uuidSize = kFrameIdentitySeiUuid.size();
    if (sideData.size < uuidSize + kPayloadSize) {
        return std::nullopt;
    }

    const std::span<const std::uint8_t> data(sideData.data, sideData.size);
    if (!std::equal(kFrameIdentitySeiUuid.begin(), kFrameIdentitySeiUuid.end(), data.begin())) {
        return std::nullopt;
    }

    const auto payload = data.subspan(uuidSize);
    if (!std::equal(kMagic.begin(), kMagic.end(), payload.begin())) {
        return std::nullopt;
    }

    const auto version = readLe16(payload, 4);
    const auto payloadSize = readLe16(payload, 6);
    if (version != kVersion || payloadSize < kPayloadSize || payload.size() < payloadSize) {
        return std::nullopt;
    }

    return SourceFrameIdentity{
        .key = FrameKey{
            .streamEpoch = readLe64(payload, 8),
            .frameId = readLe64(payload, 16),
        },
        .sourceTimeNs = readLe64(payload, 24),
    };
}

} // namespace

std::optional<SourceFrameIdentity> extractFrameIdentity(const AVFrame* frame) {
    if (frame == nullptr) {
        return std::nullopt;
    }

    for (int i = 0; i < frame->nb_side_data; ++i) {
        const AVFrameSideData* sideData = frame->side_data[i];
        if (sideData != nullptr && sideData->type == AV_FRAME_DATA_SEI_UNREGISTERED) {
            if (auto identity = parseSideData(*sideData)) {
                return identity;
            }
        }
    }
    return std::nullopt;
}

} // namespace reg::media
