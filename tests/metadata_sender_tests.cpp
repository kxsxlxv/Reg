#include "metadata/MetadataSender.hpp"
#include "metadata/Protocol.hpp"
#include "platform/UdpSocket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

reg::metadata::FrameMetadata
makeMetadata() {
    reg::metadata::FrameMetadata metadata{};
    metadata.key = {
        .streamEpoch =
            0x0102030405060708ULL,
        .frameId = 4242,
    };
    metadata.cvBeginNs = 1'000'000;
    metadata.cvEndNs = 1'400'000;
    metadata.flags = 3;

    metadata.targets.push_back(
        reg::metadata::TargetMetadata{
            .id = 77,
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

void allocationFreeEncoderMatchesVectorApi() {
    auto metadata = makeMetadata();
    metadata.sequence = 991;

    const auto vectorPacket =
        reg::metadata::protocol::
            encodeFrameMetadata(metadata);

    std::array<
        std::uint8_t,
        reg::metadata::protocol::
            kMaxDatagramSize>
        fixedPacket{};

    const std::size_t size =
        reg::metadata::protocol::
            encodeFrameMetadataInto(
                metadata,
                fixedPacket);

    require(
        size == vectorPacket.size(),
        "fixed encoder size differs from vector API");

    require(
        std::equal(
            vectorPacket.begin(),
            vectorPacket.end(),
            fixedPacket.begin()),
        "fixed encoder bytes differ from vector API");

    bool threw = false;

    try {
        std::array<std::uint8_t, 8>
            tooSmall{};

        static_cast<void>(
            reg::metadata::protocol::
                encodeFrameMetadataInto(
                    metadata,
                    tooSmall));
    } catch (const std::length_error&) {
        threw = true;
    }

    require(
        threw,
        "fixed encoder accepted undersized buffer");
}

void senderDeliversExactCvm1Datagram() {
    reg::platform::UdpSocket receiver(
        "127.0.0.1",
        0,
        1000);

    const std::uint16_t port =
        receiver.localPort();

    require(
        port != 0,
        "ephemeral receiver port was not assigned");

    reg::metadata::MetadataSender sender(
        "127.0.0.1",
        port,
        1234);

    const bool sent =
        sender.send(makeMetadata());

    require(
        sent,
        "loopback UDP sender unexpectedly would-block");

    std::array<
        std::uint8_t,
        reg::metadata::protocol::
            kMaxDatagramSize>
        receiveBuffer{};

    const std::size_t received =
        receiver.receive(
            receiveBuffer);

    require(
        received != 0,
        "loopback receiver timed out");

    const auto decoded =
        reg::metadata::protocol::
            decodeFrameMetadata(
                std::span<
                    const std::uint8_t>{
                    receiveBuffer.data(),
                    received});

    require(
        static_cast<bool>(decoded),
        "received CVM1 datagram failed validation");

    require(
        decoded.metadata.sequence == 1234,
        "sender-assigned sequence mismatch");

    require(
        decoded.metadata.key ==
            reg::media::FrameKey{
                0x0102030405060708ULL,
                4242},
        "loopback FrameKey mismatch");

    require(
        decoded.metadata.targets.size() == 1 &&
        decoded.metadata.targets[0].id == 77,
        "loopback target payload mismatch");

    const auto stats = sender.stats();

    require(
        stats.attempted == 1 &&
        stats.sent == 1 &&
        stats.droppedWouldBlock == 0 &&
        stats.nextSequence == 1235,
        "metadata sender statistics mismatch");
}

} // namespace

int main() {
    try {
        allocationFreeEncoderMatchesVectorApi();
        senderDeliversExactCvm1Datagram();

        std::cout
            << "metadata_sender_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "metadata_sender_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
