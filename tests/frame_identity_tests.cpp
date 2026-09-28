#include "media/FrameIdentity.hpp"
#include "media/FrameIdentitySei.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t kUuidSize =
    reg::media::kFrameIdentitySeiUuid.size();

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

using FramePtr =
    std::unique_ptr<AVFrame, FrameDeleter>;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

reg::media::SourceFrameIdentity sampleIdentity() {
    return reg::media::SourceFrameIdentity{
        .key = reg::media::FrameKey{
            .streamEpoch =
                0x0102030405060708ULL,
            .frameId =
                0x1122334455667788ULL,
        },
        .sourceTimeNs = 987654321ULL,
    };
}

FramePtr makeFrame(
    const std::array<std::uint8_t, 16>& uuid,
    std::span<const std::uint8_t> payload,
    std::size_t bytesToAllocate =
        kUuidSize +
        reg::media::kFrameIdentityPayloadSize) {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::bad_alloc{};
    }

    AVFrameSideData* sideData =
        av_frame_new_side_data(
            frame.get(),
            AV_FRAME_DATA_SEI_UNREGISTERED,
            bytesToAllocate);

    if (sideData == nullptr) {
        throw std::bad_alloc{};
    }

    const std::size_t uuidBytes =
        std::min(kUuidSize, bytesToAllocate);
    std::copy_n(
        uuid.data(),
        uuidBytes,
        sideData->data);

    if (bytesToAllocate > kUuidSize) {
        const std::size_t payloadBytes =
            std::min(
                payload.size(),
                bytesToAllocate - kUuidSize);

        std::copy_n(
            payload.data(),
            payloadBytes,
            sideData->data + kUuidSize);
    }

    return frame;
}

std::vector<std::uint8_t> ebspToRbsp(
    std::span<const std::uint8_t> ebsp) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(ebsp.size());

    unsigned consecutiveZeros = 0;

    for (std::size_t i = 0;
         i < ebsp.size();
         ++i) {
        const std::uint8_t byte =
            ebsp[i];

        if (consecutiveZeros >= 2U &&
            byte == 0x03U) {
            require(
                i + 1U < ebsp.size() &&
                    ebsp[i + 1U] <= 0x03U,
                "invalid emulation-prevention byte");

            consecutiveZeros = 0;
            continue;
        }

        rbsp.push_back(byte);

        if (byte == 0x00U) {
            ++consecutiveZeros;
        } else {
            consecutiveZeros = 0;
        }
    }

    return rbsp;
}

void verifySeiNalPayload(
    std::span<const std::uint8_t> nal,
    const reg::media::SourceFrameIdentity& expected) {
    require(
        !nal.empty() &&
            (nal[0] & 0x1fU) == 6U,
        "H264 NAL is not SEI type 6");

    const auto rbsp =
        ebspToRbsp(
            nal.subspan(1));

    require(
        rbsp.size() >=
            2U +
            reg::media::kFrameIdentitySeiUuid.size() +
            reg::media::kFrameIdentityPayloadSize +
            1U,
        "SEI RBSP is too short");

    require(
        rbsp[0] == 5U,
        "SEI payload type is not user_data_unregistered");

    constexpr std::size_t expectedPayloadSize =
        reg::media::kFrameIdentitySeiUuid.size() +
        reg::media::kFrameIdentityPayloadSize;

    require(
        rbsp[1] == expectedPayloadSize,
        "SEI payload size mismatch");

    require(
        std::equal(
            reg::media::kFrameIdentitySeiUuid.begin(),
            reg::media::kFrameIdentitySeiUuid.end(),
            rbsp.begin() + 2),
        "SEI UUID mismatch");

    const std::size_t payloadOffset =
        2U +
        reg::media::kFrameIdentitySeiUuid.size();

    const auto decoded =
        reg::media::decodeFrameIdentityPayload(
            std::span<const std::uint8_t>{
                rbsp.data() + payloadOffset,
                reg::media::kFrameIdentityPayloadSize});

    require(
        decoded.has_value(),
        "SEI FrameIdentity payload did not decode");
    require(
        decoded->key == expected.key,
        "SEI FrameKey mismatch");
    require(
        decoded->sourceTimeNs ==
            expected.sourceTimeNs,
        "SEI source timestamp mismatch");

    require(
        rbsp[payloadOffset +
             reg::media::kFrameIdentityPayloadSize] ==
            0x80U,
        "SEI RBSP trailing bits mismatch");
}

void annexBSeiNalRoundTrips() {
    const auto source =
        sampleIdentity();

    const auto annexB =
        reg::media::buildFrameIdentitySeiNal(
            source,
            reg::media::H264NalFraming::AnnexB);

    require(
        annexB.size() > 5U,
        "Annex-B SEI is too small");
    require(
        annexB[0] == 0x00U &&
            annexB[1] == 0x00U &&
            annexB[2] == 0x00U &&
            annexB[3] == 0x01U,
        "Annex-B start code mismatch");

    verifySeiNalPayload(
        std::span<const std::uint8_t>{
            annexB.data() + 4,
            annexB.size() - 4U},
        source);
}

void avccSeiNalRoundTrips() {
    const auto source =
        sampleIdentity();

    const auto avcc =
        reg::media::buildFrameIdentitySeiNal(
            source,
            reg::media::H264NalFraming::
                Avcc4ByteLength);

    require(
        avcc.size() > 5U,
        "AVCC SEI is too small");

    const std::uint32_t declaredSize =
        (static_cast<std::uint32_t>(
             avcc[0]) << 24U) |
        (static_cast<std::uint32_t>(
             avcc[1]) << 16U) |
        (static_cast<std::uint32_t>(
             avcc[2]) << 8U) |
        static_cast<std::uint32_t>(
            avcc[3]);

    require(
        declaredSize ==
            avcc.size() - 4U,
        "AVCC NAL length mismatch");

    verifySeiNalPayload(
        std::span<const std::uint8_t>{
            avcc.data() + 4,
            avcc.size() - 4U},
        source);
}

void seiNalUsesEmulationPrevention() {
    const reg::media::SourceFrameIdentity zeroHeavy{
        .key = {
            .streamEpoch = 0,
            .frameId = 1,
        },
        .sourceTimeNs = 0,
    };

    const auto annexB =
        reg::media::buildFrameIdentitySeiNal(
            zeroHeavy,
            reg::media::H264NalFraming::AnnexB);

    constexpr std::array<std::uint8_t, 3>
        preventionPattern{
            0x00U,
            0x00U,
            0x03U,
        };

    const bool hasPreventionByte =
        std::search(
            annexB.begin() + 5,
            annexB.end(),
            preventionPattern.begin(),
            preventionPattern.end()) !=
        annexB.end();

    require(
        hasPreventionByte,
        "zero-heavy identity did not produce H264 emulation-prevention bytes");

    verifySeiNalPayload(
        std::span<const std::uint8_t>{
            annexB.data() + 4,
            annexB.size() - 4U},
        zeroHeavy);
}

void payloadEncodingMatchesGoldenBytes() {
    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            sampleIdentity());

    const std::array<
        std::uint8_t,
        reg::media::kFrameIdentityPayloadSize>
        expected{
            'R', 'G', 'F', '1',
            0x01, 0x00,
            0x20, 0x00,

            0x08, 0x07, 0x06, 0x05,
            0x04, 0x03, 0x02, 0x01,

            0x88, 0x77, 0x66, 0x55,
            0x44, 0x33, 0x22, 0x11,

            0xb1, 0x68, 0xde, 0x3a,
            0x00, 0x00, 0x00, 0x00,
        };

    require(
        payload == expected,
        "frame identity payload bytes changed");
}

void payloadRoundTripIsStable() {
    const auto source = sampleIdentity();
    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            source);

    const auto decoded =
        reg::media::decodeFrameIdentityPayload(
            payload);

    require(
        decoded.has_value(),
        "encoded frame identity did not decode");
    require(
        decoded->key == source.key,
        "frame identity key round-trip mismatch");
    require(
        decoded->sourceTimeNs ==
            source.sourceTimeNs,
        "source timestamp round-trip mismatch");
}

void validPayloadIsDecodedFromAvFrame() {
    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            sampleIdentity());

    auto frame = makeFrame(
        reg::media::kFrameIdentitySeiUuid,
        payload);

    const auto identity =
        reg::media::extractFrameIdentity(
            frame.get());

    require(
        identity.has_value(),
        "valid Reg SEI was not decoded");
    require(
        identity->key.streamEpoch ==
            0x0102030405060708ULL,
        "stream epoch mismatch");
    require(
        identity->key.frameId ==
            0x1122334455667788ULL,
        "frame id mismatch");
    require(
        identity->sourceTimeNs ==
            987654321ULL,
        "source timestamp mismatch");
}

void foreignUuidIsIgnored() {
    auto uuid =
        reg::media::kFrameIdentitySeiUuid;
    uuid[0] ^= 0xffU;

    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            sampleIdentity());

    auto frame = makeFrame(uuid, payload);

    require(
        !reg::media::extractFrameIdentity(
             frame.get())
             .has_value(),
        "foreign SEI UUID must be ignored");
}

void unsupportedVersionIsIgnored() {
    auto payload =
        reg::media::encodeFrameIdentityPayload(
            sampleIdentity());

    payload[4] = 0x02;
    payload[5] = 0x00;

    require(
        !reg::media::decodeFrameIdentityPayload(
             payload)
             .has_value(),
        "unsupported payload version must be ignored");
}

void truncatedPayloadIsIgnored() {
    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            sampleIdentity());

    auto frame = makeFrame(
        reg::media::kFrameIdentitySeiUuid,
        payload,
        kUuidSize +
            reg::media::kFrameIdentityPayloadSize -
            1);

    require(
        !reg::media::extractFrameIdentity(
             frame.get())
             .has_value(),
        "truncated SEI payload must be ignored");
}

} // namespace

int main() {
    try {
        annexBSeiNalRoundTrips();
        avccSeiNalRoundTrips();
        seiNalUsesEmulationPrevention();
        payloadEncodingMatchesGoldenBytes();
        payloadRoundTripIsStable();
        validPayloadIsDecodedFromAvFrame();
        foreignUuidIsIgnored();
        unsupportedVersionIsIgnored();
        truncatedPayloadIsIgnored();

        std::cout
            << "frame_identity_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "frame_identity_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
