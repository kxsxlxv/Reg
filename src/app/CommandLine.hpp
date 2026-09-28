#pragma once

#include <cstdint>
#include <string>

namespace reg::app {

struct CommandLineOptions {
    std::string rtspUrl;
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
    bool validation{true};
};

CommandLineOptions parseCommandLine(int argc, char** argv);
void printUsage(const char* executableName);

} // namespace reg::app
