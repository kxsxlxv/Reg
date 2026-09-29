#pragma once

#include <cstdint>
#include <string>

namespace reg::app {

struct CommandLineOptions {
    std::string rtspUrl;
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
    bool requireFrameIdentity{false};
    std::uint64_t identityProbeFrames{0};

    std::string metadataBindAddress{"0.0.0.0"};
    std::uint16_t metadataPort{50010};
    int overlayDelayMs{150};
    bool overlayEnabled{true};

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
