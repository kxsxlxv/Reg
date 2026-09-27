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
            options.metadataBind = requireValue(i, argc, argv, "--metadata-bind");
        } else if (arg == "--metadata-port") {
            options.metadataPort = parseInteger<int>(
                requireValue(i, argc, argv, "--metadata-port"), "--metadata-port");
        } else if (arg == "--overlay-delay-ms") {
            options.overlayDelayMs = parseInteger<int>(
                requireValue(i, argc, argv, "--overlay-delay-ms"), "--overlay-delay-ms");
        } else if (arg == "--overlay-max-frames") {
            options.overlayMaxFrames = parseInteger<int>(
                requireValue(i, argc, argv, "--overlay-max-frames"), "--overlay-max-frames");
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
    if (options.metadataPort < 0 || options.metadataPort > 65535) {
        throw std::runtime_error("--metadata-port must be in [0, 65535]");
    }
    if (options.overlayDelayMs < 0) {
        throw std::runtime_error("--overlay-delay-ms must be >= 0");
    }
    if (options.overlayMaxFrames <= 0) {
        throw std::runtime_error("--overlay-max-frames must be > 0");
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
        << "  --metadata-bind IPv4     Metadata UDP bind address (default: 0.0.0.0)\n"
        << "  --metadata-port N        Enable metadata receive on UDP port N; 0 disables it\n"
        << "  --overlay-delay-ms N     Exact-sync playout delay (default: 150)\n"
        << "  --overlay-max-frames N   Bounded delayed-frame capacity (default: 32)\n"
        << "  --no-validation          Do not request VK_LAYER_KHRONOS_validation\n"
        << "  -h, --help               Show this help\n";
}

} // namespace reg::app
