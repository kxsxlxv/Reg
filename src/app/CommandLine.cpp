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
        } else if (arg == "--raw-display") {
            options.rawDisplay = parseInteger<int>(
                requireValue(i, argc, argv, "--raw-display"), "--raw-display");
        } else if (arg == "--overlay-display") {
            options.overlayDisplay = parseInteger<int>(
                requireValue(i, argc, argv, "--overlay-display"), "--overlay-display");
        } else if (arg == "--telemetry-display") {
            options.telemetryDisplay = parseInteger<int>(
                requireValue(i, argc, argv, "--telemetry-display"), "--telemetry-display");
        } else if (arg == "--netimgui-port") {
            const auto port = parseInteger<unsigned>(
                requireValue(i, argc, argv, "--netimgui-port"), "--netimgui-port");
            if (port > 65535U || port == 0) {
                throw std::runtime_error("--netimgui-port must be in [1,65535]");
            }
            options.netImguiPort = static_cast<std::uint16_t>(port);
        } else if (arg == "--disable-netimgui") {
            options.netImguiEnabled = false;
        } else if (arg == "--enable-diagnostic-api") {
            options.diagnosticApiEnabled = true;
        } else if (arg == "--diagnostic-api-port") {
            const auto port = parseInteger<unsigned>(
                requireValue(i, argc, argv, "--diagnostic-api-port"), "--diagnostic-api-port");
            if (port == 0 || port > 65535U) {
                throw std::runtime_error("--diagnostic-api-port must be in [1,65535]");
            }
            options.diagnosticApiPort = static_cast<std::uint16_t>(port);
        } else if (arg == "--require-frame-identity") {
            options.requireFrameIdentity = true;
        } else if (arg == "--identity-probe-frames") {
            options.identityProbeFrames = parseInteger<std::uint64_t>(
                requireValue(i, argc, argv, "--identity-probe-frames"),
                "--identity-probe-frames");
            options.requireFrameIdentity = true;
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
        } else if (arg == "--enable-cvv1") {
            options.cvv1Enabled = true;
        } else if (arg == "--cvv1-port") {
            const auto port = parseInteger<unsigned>(
                requireValue(i, argc, argv, "--cvv1-port"), "--cvv1-port");
            if (port == 0 || port > 65535) {
                throw std::runtime_error("--cvv1-port must be in [1,65535]");
            }
            options.cvv1Port = static_cast<std::uint16_t>(port);
        } else if (arg == "--cvv1-hide-yolo") {
            options.cvv1Observations = false;
        } else if (arg == "--cvv1-hide-candidates") {
            options.cvv1Candidates = false;
        } else if (arg == "--cvv1-hide-target") {
            options.cvv1SelectedTarget = false;
        } else if (arg == "--cvv1-hide-status") {
            options.cvv1Status = false;
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
        } else if (arg == "--disable-telemetry") {
            options.telemetryEnabled = false;
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
    if (options.rawDisplay < 0 || options.rawDisplay > 16 ||
        options.overlayDisplay < 0 || options.overlayDisplay > 16 ||
        options.telemetryDisplay < 0 || options.telemetryDisplay > 16) {
        throw std::runtime_error("Display number must be in [0,16] (0 = automatic)");
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
    if (options.cvv1Enabled && options.cvv1Port == options.metadataPort) {
        throw std::runtime_error("CVV1 and CVM1 UDP ports must differ");
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
        << "  --raw-display N          Raw monitor, 0=automatic (Windows default 3)\n"
        << "  --overlay-display N      Overlay monitor, 0=automatic (Windows default 2)\n"
        << "  --telemetry-display N    Telemetry monitor, 0=automatic (Windows default 1)\n"
        << "  --netimgui-port N        NetImgui TCP port (default: 8888)\n"
        << "  --disable-netimgui       Do not start embedded NetImgui listener\n"
        << "  --enable-diagnostic-api  Enable read-only HTTP at 127.0.0.1 (default: disabled)\n"
        << "  --diagnostic-api-port N  Diagnostic loopback port (default: 8765)\n"
        << "  --require-frame-identity Fail if a decoded frame lacks/duplicates/regresses FrameIdentity\n"
        << "  --identity-probe-frames N Validate N decoded frames then exit (implies --require-frame-identity)\n"
        << "  --metadata-bind ADDR     UDP metadata bind IPv4 address (default: 0.0.0.0)\n"
        << "  --metadata-port N        UDP metadata port (default: 50010)\n"
        << "  --enable-cvv1           Enable independent FastVision UDP receiver (default: off)\n"
        << "  --cvv1-port N           CVV1 UDP receive port (default: 50011)\n"
        << "  --cvv1-hide-yolo        Hide raw YOLO observations\n"
        << "  --cvv1-hide-candidates  Hide confirmation candidates\n"
        << "  --cvv1-hide-target      Hide selected LoRAT target\n"
        << "  --cvv1-hide-status      Hide algorithm status\n"
        << "  --overlay-delay-ms N     Exact-overlay playout delay (default: 150)\n"
        << "  --disable-overlay        Disable metadata receiver and overlay window\n"
        << "  --record-dir PATH        Rolling MKV directory (default: blackbox)\n"
        << "  --record-segment-ms N    Target segment duration (default: 5000)\n"
        << "  --record-retention-sec N Rolling retention duration (default: 300)\n"
        << "  --record-queue-capacity N Compressed packet queue bound (default: 2048)\n"
        << "  --disable-recorder       Disable rolling compressed-stream recorder\n"
        << "  --reconnect-initial-ms N Initial RTSP reconnect delay (default: 250)\n"
        << "  --reconnect-max-ms N     Maximum RTSP reconnect delay (default: 2000)\n"
        << "  --disable-telemetry      Disable the third telemetry window\n"
        << "  --no-validation          Do not request VK_LAYER_KHRONOS_validation\n"
        << "  -h, --help               Show this help\n";
}

} // namespace reg::app
