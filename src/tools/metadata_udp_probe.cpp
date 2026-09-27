#include "metadata/UdpMetadataReceiver.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t gStopRequested = 0;

void signalHandler(int) {
    gStopRequested = 1;
}

std::uint16_t parsePort(std::string_view text) {
    unsigned value = 0;
    const auto [ptr, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} ||
        ptr != text.data() + text.size() ||
        value == 0 ||
        value > 65535) {
        throw std::runtime_error(
            "UDP port must be an integer in [1, 65535]");
    }

    return static_cast<std::uint16_t>(value);
}

void printUsage(const char* executable) {
    std::cout
        << "Usage: " << executable
        << " --port N [--bind IPv4]\n\n"
        << "Receives Reg CV metadata UDP datagrams and prints validated frame metadata.\n"
        << "Default bind address: 0.0.0.0\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string bindAddress{"0.0.0.0"};
        std::uint16_t port = 0;

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];

            if (argument == "--help" || argument == "-h") {
                printUsage(argc > 0 ? argv[0] : "reg_metadata_probe");
                return 0;
            }

            if (argument == "--port") {
                if (++index >= argc) {
                    throw std::runtime_error("--port requires a value");
                }
                port = parsePort(argv[index]);
                continue;
            }

            if (argument == "--bind") {
                if (++index >= argc) {
                    throw std::runtime_error("--bind requires a value");
                }
                bindAddress = argv[index];
                continue;
            }

            throw std::runtime_error(
                "Unknown argument: " + std::string(argument));
        }

        if (port == 0) {
            throw std::runtime_error("--port is required");
        }

        std::signal(SIGINT, &signalHandler);
#ifdef SIGTERM
        std::signal(SIGTERM, &signalHandler);
#endif

        reg::metadata::UdpMetadataReceiver receiver({
            .bindAddress = bindAddress,
            .port = port,
            .receiveTimeout = std::chrono::milliseconds(100),
        });

        std::atomic_bool receiverFinished{false};
        std::mutex errorMutex;
        std::exception_ptr receiverError;

        std::cout << "[metadata] listening on "
                  << bindAddress << ':' << receiver.boundPort() << '\n';

        std::jthread thread([&] {
            try {
                receiver.run([](reg::metadata::FrameMetadataPtr metadata) {
                    std::cout
                        << "[metadata] seq=" << metadata->packetSequence
                        << " epoch=" << metadata->key.streamEpoch
                        << " frame=" << metadata->key.frameId
                        << " targets=" << metadata->targets.size();

                    if (metadata->cvBeginNs != 0 &&
                        metadata->cvEndNs >= metadata->cvBeginNs) {
                        const auto durationNs =
                            metadata->cvEndNs - metadata->cvBeginNs;
                        std::cout
                            << " cv_ms="
                            << static_cast<double>(durationNs) / 1'000'000.0;
                    }

                    std::cout << '\n';
                });
            } catch (...) {
                std::scoped_lock lock(errorMutex);
                receiverError = std::current_exception();
            }

            receiverFinished.store(true, std::memory_order_release);
        });

        while (gStopRequested == 0 &&
               !receiverFinished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        receiver.requestStop();
        thread.join();

        {
            std::scoped_lock lock(errorMutex);
            if (receiverError) {
                std::rethrow_exception(receiverError);
            }
        }

        const auto stats = receiver.stats();
        std::cout
            << "[metadata] stopped"
            << " datagrams=" << stats.datagramsReceived
            << " accepted=" << stats.packetsAccepted
            << " invalid=" << stats.invalidPackets
            << " gaps=" << stats.sequenceGaps
            << " missing_estimate=" << stats.estimatedMissingPackets
            << " duplicates=" << stats.sequenceDuplicates
            << " out_of_order=" << stats.sequenceOutOfOrder
            << " stale=" << stats.sequenceStale
            << '\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        printUsage(argc > 0 ? argv[0] : "reg_metadata_probe");
        return 1;
    }
}
