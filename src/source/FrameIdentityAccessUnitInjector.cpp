#include "source/FrameIdentityAccessUnitInjector.hpp"

#include "media/FrameIdentitySei.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace reg::source {
namespace {

struct StartCode {
    std::size_t offset{};
    std::size_t size{};
};

StartCode findStartCode(
    std::span<const std::uint8_t> bytes,
    std::size_t from) noexcept {
    for (std::size_t i = from;
         i + 2U < bytes.size();
         ++i) {
        if (bytes[i] != 0x00U ||
            bytes[i + 1U] != 0x00U) {
            continue;
        }

        if (bytes[i + 2U] == 0x01U) {
            return StartCode{
                .offset = i,
                .size = 3U,
            };
        }

        if (i + 3U < bytes.size() &&
            bytes[i + 2U] == 0x00U &&
            bytes[i + 3U] == 0x01U) {
            return StartCode{
                .offset = i,
                .size = 4U,
            };
        }
    }

    return StartCode{
        .offset = bytes.size(),
        .size = 0U,
    };
}

bool isVclNalType(
    std::uint8_t nalType) noexcept {
    // H.264 VCL NAL unit types used by ordinary AVC plus the extension slice
    // types defined by later profiles.
    return (nalType >= 1U &&
            nalType <= 5U) ||
           nalType == 19U ||
           nalType == 20U ||
           nalType == 21U;
}

std::size_t firstVclStartCodeOffset(
    std::span<const std::uint8_t> accessUnit) {
    std::size_t searchFrom = 0;

    while (true) {
        const StartCode start =
            findStartCode(
                accessUnit,
                searchFrom);

        if (start.size == 0U) {
            break;
        }

        const std::size_t headerOffset =
            start.offset + start.size;

        if (headerOffset >= accessUnit.size()) {
            throw std::invalid_argument(
                "Annex-B access unit ends after a start code");
        }

        const std::uint8_t nalType =
            static_cast<std::uint8_t>(
                accessUnit[headerOffset] &
                0x1fU);

        if (isVclNalType(nalType)) {
            return start.offset;
        }

        searchFrom =
            headerOffset + 1U;
    }

    throw std::invalid_argument(
        "Annex-B access unit contains no H.264 VCL NAL");
}

} // namespace

std::uint64_t generateStreamEpoch() {
    std::random_device random;

    std::uint64_t epoch =
        static_cast<std::uint64_t>(
            random());

    epoch =
        (epoch << 32U) ^
        static_cast<std::uint64_t>(
            random());

    epoch ^=
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    // Zero is not technically forbidden by the wire format, but reserving it
    // as an obvious uninitialized value makes source-side diagnostics safer.
    if (epoch == 0U) {
        epoch = 1U;
    }

    return epoch;
}

void injectFrameIdentitySeiIntoAnnexBAccessUnit(
    const media::SourceFrameIdentity& identity,
    std::span<const std::uint8_t> encodedAccessUnit,
    std::vector<std::uint8_t>& output) {
    if (encodedAccessUnit.empty()) {
        throw std::invalid_argument(
            "encoded Annex-B access unit is empty");
    }

    const std::size_t insertionOffset =
        firstVclStartCodeOffset(
            encodedAccessUnit);

    const auto sei =
        media::buildFrameIdentitySeiNal(
            identity,
            media::H264NalFraming::AnnexB);

    output.clear();
    output.reserve(
        encodedAccessUnit.size() +
        sei.size());

    output.insert(
        output.end(),
        encodedAccessUnit.begin(),
        encodedAccessUnit.begin() +
            static_cast<std::ptrdiff_t>(
                insertionOffset));

    output.insert(
        output.end(),
        sei.begin(),
        sei.end());

    output.insert(
        output.end(),
        encodedAccessUnit.begin() +
            static_cast<std::ptrdiff_t>(
                insertionOffset),
        encodedAccessUnit.end());
}

FrameIdentityAccessUnitInjector::
FrameIdentityAccessUnitInjector(
    std::uint64_t streamEpoch,
    std::uint64_t firstFrameId)
    : streamEpoch_(streamEpoch),
      nextFrameId_(firstFrameId) {
    if (streamEpoch_ == 0U) {
        throw std::invalid_argument(
            "stream epoch must be non-zero");
    }
}

media::SourceFrameIdentity
FrameIdentityAccessUnitInjector::injectNext(
    std::span<const std::uint8_t> encodedAccessUnit,
    std::uint64_t sourceTimeNs,
    std::vector<std::uint8_t>& output) {
    if (nextFrameId_ ==
        std::numeric_limits<
            std::uint64_t>::max()) {
        throw std::overflow_error(
            "source frame_id exhausted");
    }

    const media::SourceFrameIdentity identity{
        .key = {
            .streamEpoch = streamEpoch_,
            .frameId = nextFrameId_,
        },
        .sourceTimeNs = sourceTimeNs,
    };

    injectFrameIdentitySeiIntoAnnexBAccessUnit(
        identity,
        encodedAccessUnit,
        output);

    ++nextFrameId_;
    return identity;
}

} // namespace reg::source
