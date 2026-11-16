#pragma once

#include <cstdint>
#include <string>

namespace reg::app {

enum class GtCapturePolicy { Live, Benchmark };

struct CommandLineOptions {
    std::string rtspUrl;
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
    // 0 retains existing placement; 1..16 selects Windows Settings
    // display number, or a 1-based SDL display ordinal on Linux.
    int rawDisplay{0};
    int overlayDisplay{0};
    int telemetryDisplay{0};
    // Prevent Reg windows from appearing on an output switched to Jetson.
    int reservedDisplay{0};
    std::uint16_t netImguiPort{8888};
    bool netImguiEnabled{true};
    // Read-only Diagnostic API listener is opt-in and loopback-only.
    bool diagnosticApiEnabled{false};
    std::uint16_t diagnosticApiPort{8765};
    bool requireFrameIdentity{false};
    std::uint64_t identityProbeFrames{0};

    std::string metadataBindAddress{"0.0.0.0"};
    std::uint16_t metadataPort{50010};
    int overlayDelayMs{150};
    bool overlayEnabled{true};

    // Live keeps newest-frame display. Benchmark retains all original
    // compressed H264 AUs/SEI before leaky display queues, if recorder succeeds.
    GtCapturePolicy gtCapturePolicy{GtCapturePolicy::Live};
    bool recorderEnabled{true};
    std::string recordDirectory{"blackbox"};
    int recordSegmentMs{5000};
    int recordRetentionSeconds{300};
    int recordQueueCapacity{2048};

    int reconnectInitialMs{250};
    int reconnectMaxMs{2000};

    bool telemetryEnabled{true};
    bool validation{true};
};

CommandLineOptions parseCommandLine(int argc, char** argv);
void printUsage(const char* executableName);

} // namespace reg::app
