#include "metadata/MetadataProtocol.hpp"
#include "metadata/UdpMetadataReceiver.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

reg::metadata::FrameMetadata makeMetadata(
    std::uint32_t sequence,
    std::uint64_t frameId) {
    reg::metadata::FrameMetadata metadata;
    metadata.key = {.streamEpoch = 4, .frameId = frameId};
    metadata.packetSequence = sequence;
    metadata.cvBeginNs = 1000;
    metadata.cvEndNs = 2000;
    metadata.targets.push_back(reg::metadata::TargetMetadata{
        .id = frameId,
        .classId = 2,
        .confidence = 0.8F,
        .bbox = {
            .x = 0.1F,
            .y = 0.2F,
            .width = 0.3F,
            .height = 0.4F,
        },
    });
    return metadata;
}

void sendDatagram(
    std::uint16_t port,
    const std::vector<std::uint8_t>& bytes) {
#ifdef _WIN32
    const SOCKET sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == INVALID_SOCKET) {
        throw std::runtime_error("test sender socket creation failed");
    }
#else
    const int sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender < 0) {
        throw std::runtime_error("test sender socket creation failed");
    }
#endif

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    require(
        inet_pton(AF_INET, "127.0.0.1", &target.sin_addr) == 1,
        "inet_pton failed");

#ifdef _WIN32
    const int result = sendto(
        sender,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        static_cast<int>(sizeof(target)));
    closesocket(sender);
    require(result == static_cast<int>(bytes.size()), "sendto failed");
#else
    const ssize_t result = sendto(
        sender,
        bytes.data(),
        bytes.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
    ::close(sender);
    require(
        result == static_cast<ssize_t>(bytes.size()),
        "sendto failed");
#endif
}

void receiverAcceptsValidPacketsAndReportsTransportAnomalies() {
    reg::metadata::UdpMetadataReceiver receiver({
        .bindAddress = "127.0.0.1",
        .port = 0,
        .receiveTimeout = 20ms,
    });

    require(receiver.boundPort() != 0, "receiver did not expose its ephemeral port");

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<reg::metadata::FrameMetadataPtr> received;

    std::jthread thread([&] {
        receiver.run([&](reg::metadata::FrameMetadataPtr metadata) {
            {
                std::scoped_lock lock(mutex);
                received.push_back(std::move(metadata));
            }
            cv.notify_all();
        });
    });

    const auto packet10 =
        reg::metadata::wire::encodeFrameMetadata(makeMetadata(10, 100));
    const auto packet10Duplicate =
        reg::metadata::wire::encodeFrameMetadata(makeMetadata(10, 100));
    const auto packet12 =
        reg::metadata::wire::encodeFrameMetadata(makeMetadata(12, 102));

    auto corrupted =
        reg::metadata::wire::encodeFrameMetadata(makeMetadata(13, 103));
    corrupted.at(reg::metadata::wire::kHeaderSize + 1) ^= 0x40U;

    sendDatagram(receiver.boundPort(), packet10);
    sendDatagram(receiver.boundPort(), packet10Duplicate);
    sendDatagram(receiver.boundPort(), packet12);
    sendDatagram(receiver.boundPort(), corrupted);

    {
        std::unique_lock lock(mutex);
        const bool complete = cv.wait_for(lock, 2s, [&] {
            return received.size() >= 3;
        });
        require(complete, "receiver did not deliver expected valid packets");
    }

    receiver.requestStop();
    thread.join();

    {
        std::scoped_lock lock(mutex);
        require(received.size() == 3, "invalid packet reached callback");
        require(received[0]->packetSequence == 10, "first packet sequence mismatch");
        require(received[1]->packetSequence == 10, "duplicate packet sequence mismatch");
        require(received[2]->packetSequence == 12, "gap packet sequence mismatch");
    }

    const auto stats = receiver.stats();
    require(stats.datagramsReceived == 4, "datagram counter mismatch");
    require(stats.packetsAccepted == 3, "accepted packet counter mismatch");
    require(stats.invalidPackets == 1, "invalid packet counter mismatch");
    require(stats.sequenceDuplicates == 1, "duplicate sequence counter mismatch");
    require(stats.sequenceGaps == 1, "sequence gap counter mismatch");
    require(stats.estimatedMissingPackets == 1, "estimated missing packet count mismatch");
}

} // namespace

int main() {
    try {
        receiverAcceptsValidPacketsAndReportsTransportAnomalies();

        std::cout << "udp_metadata_receiver_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "udp_metadata_receiver_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
