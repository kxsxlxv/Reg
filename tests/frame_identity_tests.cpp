#include "media/FrameIdentity.hpp"

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
