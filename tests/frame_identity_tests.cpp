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

constexpr std::size_t kUuidSize = reg::media::kFrameIdentitySeiUuid.size();
constexpr std::size_t kPayloadSize = 32;

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

void writeLe16(std::uint8_t* bytes, std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void writeLe64(std::uint8_t* bytes, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU);
    }
}

FramePtr makeFrame(
    const std::array<std::uint8_t, 16>& uuid,
    std::uint16_t version,
    std::size_t bytesToAllocate = kUuidSize + kPayloadSize) {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::bad_alloc{};
    }

    AVFrameSideData* sideData =
        av_frame_new_side_data(frame.get(), AV_FRAME_DATA_SEI_UNREGISTERED, bytesToAllocate);
    if (sideData == nullptr) {
        throw std::bad_alloc{};
    }

    const std::size_t uuidBytes = std::min(kUuidSize, bytesToAllocate);
    std::copy_n(uuid.data(), uuidBytes, sideData->data);

    if (bytesToAllocate >= kUuidSize + kPayloadSize) {
        auto* payload = sideData->data + kUuidSize;
        payload[0] = 'R';
        payload[1] = 'G';
        payload[2] = 'F';
        payload[3] = '1';

        writeLe16(payload + 4, version);
        writeLe16(payload + 6, static_cast<std::uint16_t>(kPayloadSize));

        writeLe64(payload + 8, 0x0102030405060708ULL);
        writeLe64(payload + 16, 0x1122334455667788ULL);
        writeLe64(payload + 24, 987654321ULL);
    }

    return frame;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void validPayloadIsDecoded() {
    auto frame = makeFrame(reg::media::kFrameIdentitySeiUuid, 1);
    const auto identity = reg::media::extractFrameIdentity(frame.get());

    require(identity.has_value(), "valid Reg SEI was not decoded");
    require(identity->key.streamEpoch == 0x0102030405060708ULL, "stream epoch mismatch");
    require(identity->key.frameId == 0x1122334455667788ULL, "frame id mismatch");
    require(identity->sourceTimeNs == 987654321ULL, "source timestamp mismatch");
}

void foreignUuidIsIgnored() {
    auto uuid = reg::media::kFrameIdentitySeiUuid;
    uuid[0] ^= 0xffU;

    auto frame = makeFrame(uuid, 1);
    require(
        !reg::media::extractFrameIdentity(frame.get()).has_value(),
        "foreign SEI UUID must be ignored");
}

void unsupportedVersionIsIgnored() {
    auto frame = makeFrame(reg::media::kFrameIdentitySeiUuid, 2);
    require(
        !reg::media::extractFrameIdentity(frame.get()).has_value(),
        "unsupported SEI version must be ignored");
}

void truncatedPayloadIsIgnored() {
    auto frame = makeFrame(
        reg::media::kFrameIdentitySeiUuid,
        1,
        kUuidSize + kPayloadSize - 1);

    require(
        !reg::media::extractFrameIdentity(frame.get()).has_value(),
        "truncated SEI payload must be ignored");
}

} // namespace

int main() {
    try {
        validPayloadIsDecoded();
        foreignUuidIsIgnored();
        unsupportedVersionIsIgnored();
        truncatedPayloadIsIgnored();
        std::cout << "frame_identity_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "frame_identity_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
