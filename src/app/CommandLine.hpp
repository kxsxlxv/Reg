#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reg::app {

struct CommandLineOptions {
    std::string rtspUrl;
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};

    std::string metadataBind{"0.0.0.0"};
    std::uint16_t metadataPort{0};
    std::size_t metadataCapacity{512};
    std::size_t overlayBufferFrames{32};
    int overlayDelayMs{150};

    bool validation{true};
};

CommandLineOptions parseCommandLine(int argc, char** argv);
void printUsage(const char* executableName);

} // namespace reg::app
