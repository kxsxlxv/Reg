#include "metadata/MetadataReceiver.hpp"
#include "metadata/MetadataStore.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t gSignalStopRequested = 0;

void signalHandler(int) {
    gSignalStopRequested = 1;
}

struct Options {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{5000};
    std::size_t capacity{512};
    std::uint32_t durationSeconds{0};
};

template <typename T>
T parseInteger(std::string_view text, const char* name) {
    T value{};
    const auto [ptr, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error(
            std::string("Invalid value for ") + name + ": " + std::string(text));
    }
    return value;
}

std::string_view requireValue(
    int& index,
    int argc,
    char** argv,
    const char* option) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + option);
    }
    ++index;
    return argv[index];
}

void printUsage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n\n"
        << "Options:\n"
        << "  --bind ADDRESS        IPv4 bind address (default: 0.0.0.0)\n"
        << "  --port N              UDP port (default: 5000)\n"
        << "  --capacity N          MetadataStore capacity (default: 512)\n"
        << "  --duration-sec N      Stop after N seconds; 0 = until Ctrl+C\n"
        << "  -h, --help            Show this help\n";
}

Options parseOptions(int argc, char** argv) {
    Options options{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--bind") {
            options.bindAddress = requireValue(i, argc, argv, "--bind");
        } else if (arg == "--port") {
            const auto value =
                parseInteger<std::uint32_t>(
                    requireValue(i, argc, argv, "--port"),
                    "--port");
            if (value == 0 || value > 65535) {
                throw std::runtime_error("--port must be in range 1..65535");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (arg == "--capacity") {
            options.capacity =
                parseInteger<std::size_t>(
                    requireValue(i, argc, argv, "--capacity"),
                    "--capacity");
            if (options.capacity == 0) {
                throw std::runtime_error("--capacity must be greater than zero");
            }
        } else if (arg == "--duration-sec") {
            options.durationSeconds =
                parseInteger<std::uint32_t>(
                    requireValue(i, argc, argv, "--duration-sec"),
                    "--duration-sec");
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argc > 0 ? argv[0] : "reg_metadata_probe");
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg));
        }
    }

    return options;
}

void printStats(
    const reg::metadata::MetadataReceiverStats& stats,
    std::size_t storeSize) {
    std::cout
        << "[metadata] rx=" << stats.datagramsReceived
        << " accepted=" << stats.packetsAccepted
        << " invalid=" << stats.invalidPackets
        << " duplicate_seq=" << stats.duplicateSequences
        << " duplicate_frame=" << stats.duplicateFrames
        << " out_of_order=" << stats.outOfOrderSequences
        << " gaps_observed=" << stats.sequenceGapsObserved
        << " late=" << stats.latePackets
        << " store_evictions=" << stats.storeEvictions
        << " store_size=" << storeSize
        << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);

        std::signal(SIGINT, signalHandler);
#ifdef SIGTERM
        std::signal(SIGTERM, signalHandler);
#endif

        reg::metadata::MetadataStore store(options.capacity);
        reg::metadata::MetadataReceiver receiver(
            store,
            reg::metadata::MetadataReceiverConfig{
                .bindAddress = options.bindAddress,
                .port = options.port,
                .pollTimeout = 20ms,
            });

        std::cout
            << "[metadata] listening on "
            << options.bindAddress << ':' << options.port
            << " capacity=" << options.capacity << '\n';

        std::mutex errorMutex;
        std::exception_ptr receiverError;
        std::atomic_bool receiverFailed{false};

        std::jthread receiverThread([&] {
            try {
                receiver.run();
            } catch (...) {
                std::scoped_lock lock(errorMutex);
                receiverError = std::current_exception();
                receiverFailed.store(true, std::memory_order_release);
            }
        });

        const auto startedAt = std::chrono::steady_clock::now();
        auto nextReport = startedAt + 1s;

        while (gSignalStopRequested == 0 &&
               !receiverFailed.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();

            if (options.durationSeconds != 0 &&
                now - startedAt >= std::chrono::seconds(options.durationSeconds)) {
                break;
            }

            if (now >= nextReport) {
                printStats(receiver.stats(), store.size());
                nextReport += 1s;
            }

            std::this_thread::sleep_for(20ms);
        }

        receiver.requestStop();
        if (receiverThread.joinable()) {
            receiverThread.join();
        }

        {
            std::scoped_lock lock(errorMutex);
            if (receiverError) {
                std::rethrow_exception(receiverError);
            }
        }

        printStats(receiver.stats(), store.size());
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        printUsage(argc > 0 ? argv[0] : "reg_metadata_probe");
        return EXIT_FAILURE;
    }
}
