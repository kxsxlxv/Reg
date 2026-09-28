#include "media/FrameIdentitySei.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace reg::media {
namespace {

constexpr std::uint8_t kSeiNalHeader = 0x06;
constexpr std::uint8_t kUserDataUnregisteredPayloadType = 5;

void appendSeiScalar(
    std::vector<std::uint8_t>& rbsp,
    std::size_t value) {
    while (value >= 255U) {
        rbsp.push_back(0xffU);
        value -= 255U;
    }

    rbsp.push_back(
        static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> rbspToEbsp(
    const std::vector<std::uint8_t>& rbsp) {
    std::vector<std::uint8_t> ebsp;
    ebsp.reserve(
        rbsp.size() +
        rbsp.size() / 16U);

    unsigned consecutiveZeros = 0;

    for (const std::uint8_t byte : rbsp) {
        if (consecutiveZeros >= 2U &&
            byte <= 0x03U) {
            ebsp.push_back(0x03U);
            consecutiveZeros = 0;
        }

        ebsp.push_back(byte);

        if (byte == 0x00U) {
            ++consecutiveZeros;
        } else {
            consecutiveZeros = 0;
        }
    }

    return ebsp;
}

void appendBigEndianU32(
    std::vector<std::uint8_t>& output,
    std::uint32_t value) {
    output.push_back(
        static_cast<std::uint8_t>(
            (value >> 24U) & 0xffU));
    output.push_back(
        static_cast<std::uint8_t>(
            (value >> 16U) & 0xffU));
    output.push_back(
        static_cast<std::uint8_t>(
            (value >> 8U) & 0xffU));
    output.push_back(
        static_cast<std::uint8_t>(
            value & 0xffU));
}

} // namespace

std::vector<std::uint8_t> buildFrameIdentitySeiNal(
    const SourceFrameIdentity& identity,
    H264NalFraming framing) {
    const auto payload =
        encodeFrameIdentityPayload(
            identity);

    constexpr std::size_t userPayloadSize =
        kFrameIdentitySeiUuid.size() +
        kFrameIdentityPayloadSize;

    static_assert(
        userPayloadSize == 48U);

    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(
        2U +
        userPayloadSize +
        1U);

    appendSeiScalar(
        rbsp,
        kUserDataUnregisteredPayloadType);

    appendSeiScalar(
        rbsp,
        userPayloadSize);

    rbsp.insert(
        rbsp.end(),
        kFrameIdentitySeiUuid.begin(),
        kFrameIdentitySeiUuid.end());

    rbsp.insert(
        rbsp.end(),
        payload.begin(),
        payload.end());

    // rbsp_trailing_bits: one stop bit followed by zero padding.
    rbsp.push_back(0x80U);

    const auto ebsp =
        rbspToEbsp(rbsp);

    const std::size_t nalSize =
        1U + ebsp.size();

    if (nalSize >
        std::numeric_limits<
            std::uint32_t>::max()) {
        throw std::overflow_error(
            "FrameIdentity SEI NAL is too large");
    }

    std::vector<std::uint8_t> output;
    output.reserve(
        nalSize + 4U);

    switch (framing) {
    case H264NalFraming::AnnexB:
        output.insert(
            output.end(),
            {0x00U, 0x00U, 0x00U, 0x01U});
        break;

    case H264NalFraming::Avcc4ByteLength:
        appendBigEndianU32(
            output,
            static_cast<std::uint32_t>(
                nalSize));
        break;
    }

    output.push_back(
        kSeiNalHeader);

    output.insert(
        output.end(),
        ebsp.begin(),
        ebsp.end());

    return output;
}

} // namespace reg::media
