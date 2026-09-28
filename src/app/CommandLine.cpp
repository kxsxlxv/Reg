#include "app/CommandLine.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace reg::app {
namespace {

template <typename T>
T parseInteger(std::string_view text, const char* name) {
    T value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::runtime_error(std::string("Invalid value for ") + name + ": " + std::string(text));
    }
    return value;
}

std::string_view requireValue(int& index, int argc, char** argv, const char* option) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + option);
    }
    ++index;
    return argv[index];
}

} // namespace

CommandLineOptions parseCommandLine(int argc, char** argv) {
    CommandLineOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--url" || arg == "--rtsp-url") {
            options.rtspUrl = requireValue(i, argc, argv, "--url");
        } else if (arg == "--max-delay-us") {
            options.maxDelayUs = parseInteger<std::int64_t>(
                requireValue(i, argc, argv, "--max-delay-us"), "--max-delay-us");
        } else if (arg == "--reorder-queue-size") {
            options.reorderQueueSize = parseInteger<int>(
                requireValue(i, argc, argv, "--reorder-queue-size"), "--reorder-queue-size");
        } else if (arg == "--extra-hw-frames") {
            options.extraHwFrames = parseInteger<int>(
                requireValue(i, argc, argv, "--extra-hw-frames"), "--extra-hw-frames");
        } else if (arg == "--metadata-bind") {
            options.metadataBindAddress =
                requireValue(i, argc, argv, "--metadata-bind");
        } else if (arg == "--metadata-port") {
            const auto port = parseInteger<unsigned>(
                requireValue(i, argc, argv, "--metadata-port"), "--metadata-port");
            if (port > 65535U) {
                throw std::runtime_error("--metadata-port must be <= 65535");
            }
            options.metadataPort = static_cast<std::uint16_t>(port);
        } else if (arg == "--overlay-delay-ms") {
            options.overlayDelayMs = parseInteger<int>(
                requireValue(i, argc, argv, "--overlay-delay-ms"), "--overlay-delay-ms");
        } else if (arg == "--disable-overlay") {
            options.overlayEnabled = false;
        } else if (arg == "--record-dir") {
            options.recordDirectory =
                requireValue(i, argc, argv, "--record-dir");
        } else if (arg == "--record-segment-ms") {
            options.recordSegmentMs = parseInteger<int>(
                requireValue(i, argc, argv, "--record-segment-ms"), "--record-segment-ms");
        } else if (arg == "--record-retention-sec") {
            options.recordRetentionSeconds = parseInteger<int>(
                requireValue(i, argc, argv, "--record-retention-sec"), "--record-retention-sec");
        } else if (arg == "--record-queue-capacity") {
            options.recordQueueCapacity = parseInteger<int>(
                requireValue(i, argc, argv, "--record-queue-capacity"), "--record-queue-capacity");
        } else if (arg == "--disable-recorder") {
            options.recorderEnabled = false;
        } else if (arg == "--reconnect-initial-ms") {
            options.reconnectInitialMs = parseInteger<int>(
                requireValue(i, argc, argv, "--reconnect-initial-ms"), "--reconnect-initial-ms");
        } else if (arg == "--reconnect-max-ms") {
            options.reconnectMaxMs = parseInteger<int>(
                requireValue(i, argc, argv, "--reconnect-max-ms"), "--reconnect-max-ms");
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg));
        }
    }

    if (options.rtspUrl.empty()) {
        throw std::runtime_error("RTSP URL is required. Use --url rtsp://...");
    }
    if (options.maxDelayUs < 0) {
        throw std::runtime_error("--max-delay-us must be >= 0");
    }
    if (options.reorderQueueSize < 0) {
        throw std::runtime_error("--reorder-queue-size must be >= 0");
    }
    if (options.extraHwFrames < 0) {
        throw std::runtime_error("--extra-hw-frames must be >= 0");
    }
    if (options.metadataPort == 0) {
        throw std::runtime_error("--metadata-port must be in [1,65535]");
    }
    if (options.overlayDelayMs < 0) {
        throw std::runtime_error("--overlay-delay-ms must be >= 0");
    }
    if (options.recordDirectory.empty()) {
        throw std::runtime_error("--record-dir must not be empty");
    }
    if (options.recordSegmentMs <= 0) {
        throw std::runtime_error("--record-segment-ms must be > 0");
    }
    if (options.recordRetentionSeconds <= 0) {
        throw std::runtime_error("--record-retention-sec must be > 0");
    }
    if (options.recordRetentionSeconds * 1000 <
        options.recordSegmentMs) {
        throw std::runtime_error(
            "record retention must be at least one segment");
    }
    if (options.recordQueueCapacity <= 0) {
        throw std::runtime_error("--record-queue-capacity must be > 0");
    }
    if (options.reconnectInitialMs <= 0) {
        throw std::runtime_error("--reconnect-initial-ms must be > 0");
    }
    if (options.reconnectMaxMs < options.reconnectInitialMs) {
        throw std::runtime_error("--reconnect-max-ms must be >= --reconnect-initial-ms");
    }

    return options;
}

void printUsage(const char* executableName) {
    std::cout
        << "Usage: " << executableName << " --url rtsp://... [options]\n\n"
        << "Options:\n"
        << "  --max-delay-us N         FFmpeg RTSP demux max_delay (default: 0)\n"
        << "  --reorder-queue-size N   RTP packet reorder queue size (default: 0)\n"
        << "  --extra-hw-frames N      Extra Vulkan decode surfaces (default: 32)\n"
        << "  --metadata-bind ADDR     UDP metadata bind IPv4 address (default: 0.0.0.0)\n"
        << "  --metadata-port N        UDP metadata port (default: 50010)\n"
        << "  --overlay-delay-ms N     Exact-overlay playout delay (default: 150)\n"
        << "  --disable-overlay        Disable metadata receiver and overlay window\n"
        << "  --record-dir PATH        Rolling MKV directory (default: blackbox)\n"
        << "  --record-segment-ms N    Target segment duration (default: 5000)\n"
        << "  --record-retention-sec N Rolling retention duration (default: 300)\n"
        << "  --record-queue-capacity N Compressed packet queue bound (default: 2048)\n"
        << "  --disable-recorder       Disable rolling compressed-stream recorder\n"
        << "  --reconnect-initial-ms N Initial RTSP reconnect delay (default: 250)\n"
        << "  --reconnect-max-ms N     Maximum RTSP reconnect delay (default: 2000)\n"
        << "  --no-validation          Do not request VK_LAYER_KHRONOS_validation\n"
        << "  -h, --help               Show this help\n";
}

} // namespace reg::app
