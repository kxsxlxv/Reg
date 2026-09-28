#include "jetson/MetadataBridge.hpp"
#include "media/FrameIdentity.hpp"
#include "metadata/Protocol.hpp"
#include "platform/UdpSocket.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

using FramePtr =
    std::unique_ptr<AVFrame, FrameDeleter>;

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

reg::media::SourceFrameIdentity
makeIdentity(
    std::uint64_t frameId) {
    return reg::media::SourceFrameIdentity{
        .key = {
            .streamEpoch =
                0x0102030405060708ULL,
            .frameId = frameId,
        },
        .sourceTimeNs =
            5'000'000ULL + frameId,
    };
}

FramePtr makeFrame(
    const reg::media::SourceFrameIdentity& identity) {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::bad_alloc{};
    }

    const auto payload =
        reg::media::encodeFrameIdentityPayload(
            identity);

    constexpr std::size_t uuidSize =
        reg::media::kFrameIdentitySeiUuid.size();

    AVFrameSideData* sideData =
        av_frame_new_side_data(
            frame.get(),
            AV_FRAME_DATA_SEI_UNREGISTERED,
            uuidSize + payload.size());

    if (sideData == nullptr) {
        throw std::bad_alloc{};
    }

    std::copy(
        reg::media::kFrameIdentitySeiUuid.begin(),
        reg::media::kFrameIdentitySeiUuid.end(),
        sideData->data);

    std::copy(
        payload.begin(),
        payload.end(),
        sideData->data + uuidSize);

    return frame;
}

FramePtr makeFrameWithoutIdentity() {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::bad_alloc{};
    }

    return frame;
}

reg::metadata::FrameMetadata
makeResult(
    std::uint64_t targetId) {
    reg::metadata::FrameMetadata metadata{};

    // Deliberately wrong: MetadataBridge must replace this with the key that
    // came from the decoded frame's Reg SEI.
    metadata.key = {
        .streamEpoch = 0xffffffffffffffffULL,
        .frameId = 0xffffffffffffffffULL,
    };
    metadata.cvBeginNs = 999;
    metadata.cvEndNs = 999;
    metadata.flags = 3;

    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = targetId,
            .classId = 5,
            .flags = 9,
            .confidence = 0.875F,
            .bbox = {
                .x = 0.10F,
                .y = 0.20F,
                .width = 0.30F,
                .height = 0.40F,
            },
        });

    return metadata;
}

reg::metadata::FrameMetadata
receiveOne(
    reg::platform::UdpSocket& receiver) {
    std::array<
        std::uint8_t,
        reg::metadata::protocol::
            kMaxDatagramSize>
        buffer{};

    const std::size_t received =
        receiver.receive(buffer);

    require(
        received != 0,
        "loopback receiver timed out");

    const auto decoded =
        reg::metadata::protocol::
            decodeFrameMetadata(
                std::span<
                    const std::uint8_t>{
                    buffer.data(),
                    received});

    require(
        static_cast<bool>(decoded),
        "received CVM1 datagram failed validation");

    return decoded.metadata;
}

void exactIdentityFlowsFromAvFrameToCvm1() {
    reg::platform::UdpSocket receiver(
        "127.0.0.1",
        0,
        1000);

    reg::jetson::MetadataBridge bridge(
        "127.0.0.1",
        receiver.localPort(),
        1234);

    const auto identity =
        makeIdentity(4242);
    auto frame =
        makeFrame(identity);

    const auto context =
        bridge.beginFrame(
            frame.get(),
            10'000'000);

    require(
        context.has_value(),
        "valid Reg frame identity was not captured");
    require(
        context->key() == identity.key,
        "captured FrameKey mismatch");
    require(
        context->sourceTimeNs() ==
            identity.sourceTimeNs,
        "captured source timestamp mismatch");

    const bool sent =
        bridge.sendResult(
            *context,
            10'400'000,
            makeResult(77));

    require(
        sent,
        "loopback metadata send unexpectedly would-block");

    const auto received =
        receiveOne(receiver);

    require(
        received.key == identity.key,
        "CVM1 key did not come from decoded frame identity");
    require(
        received.sequence == 1234,
        "transport sequence mismatch");
    require(
        received.cvBeginNs == 10'000'000 &&
            received.cvEndNs == 10'400'000,
        "CV timing did not come from bridge context/completion");
    require(
        received.targets.size() == 1 &&
            received.targets[0].id == 77,
        "target result was not preserved");

    const auto stats =
        bridge.stats();

    require(
        stats.decodedFramesObserved == 1 &&
            stats.framesMissingIdentity == 0,
        "bridge identity statistics mismatch");
    require(
        stats.transport.attempted == 1 &&
            stats.transport.sent == 1 &&
            stats.transport.nextSequence == 1235,
        "bridge transport statistics mismatch");
}

void missingIdentityDoesNotCreateContext() {
    reg::platform::UdpSocket receiver(
        "127.0.0.1",
        0,
        50);

    reg::jetson::MetadataBridge bridge(
        "127.0.0.1",
        receiver.localPort());

    auto frame =
        makeFrameWithoutIdentity();

    const auto context =
        bridge.beginFrame(
            frame.get(),
            20'000'000);

    require(
        !context.has_value(),
        "frame without Reg SEI must not get a synthetic key");

    const auto stats =
        bridge.stats();

    require(
        stats.decodedFramesObserved == 1 &&
            stats.framesMissingIdentity == 1,
        "missing-identity statistics mismatch");
    require(
        stats.transport.attempted == 0,
        "missing identity must not emit metadata");
}

void outOfOrderCompletionKeepsExactFrameKeys() {
    reg::platform::UdpSocket receiver(
        "127.0.0.1",
        0,
        1000);

    reg::jetson::MetadataBridge bridge(
        "127.0.0.1",
        receiver.localPort(),
        900);

    const auto firstIdentity =
        makeIdentity(100);
    const auto secondIdentity =
        makeIdentity(101);

    auto firstFrame =
        makeFrame(firstIdentity);
    auto secondFrame =
        makeFrame(secondIdentity);

    const auto first =
        bridge.beginFrame(
            firstFrame.get(),
            30'000'000);
    const auto second =
        bridge.beginFrame(
            secondFrame.get(),
            31'000'000);

    require(
        first.has_value() &&
            second.has_value(),
        "test identities were not captured");

    // Simulate variable inference latency: frame 101 completes before 100.
    require(
        bridge.sendResult(
            *second,
            31'500'000,
            makeResult(1010)),
        "second result send failed");
    require(
        bridge.sendResult(
            *first,
            32'500'000,
            makeResult(1000)),
        "first result send failed");

    const auto packetOne =
        receiveOne(receiver);
    const auto packetTwo =
        receiveOne(receiver);

    require(
        packetOne.sequence == 900 &&
            packetOne.key ==
                secondIdentity.key,
        "first completed result lost frame 101 identity");
    require(
        packetTwo.sequence == 901 &&
            packetTwo.key ==
                firstIdentity.key,
        "second completed result lost frame 100 identity");

    require(
        packetOne.key.frameId >
            packetTwo.key.frameId,
        "test did not preserve out-of-order frame completion");
}

} // namespace

int main() {
    try {
        exactIdentityFlowsFromAvFrameToCvm1();
        missingIdentityDoesNotCreateContext();
        outOfOrderCompletionKeepsExactFrameKeys();

        std::cout
            << "jetson_metadata_bridge_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "jetson_metadata_bridge_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
