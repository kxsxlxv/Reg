#include "app/CommandLine.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
reg::app::CommandLineOptions parse(std::vector<std::string> inputs) {
    std::vector<char*> args;
    for (auto& item : inputs) args.push_back(item.data());
    return reg::app::parseCommandLine(
        static_cast<int>(args.size()), args.data());
}
bool fails(std::vector<std::string> inputs) {
    try {
        static_cast<void>(parse(std::move(inputs)));
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}
}

int main() {
    const auto defaults = parse({"probe", "--url", "rtsp://localhost/stream"});
    assert(defaults.rawDisplay == 0);
    assert(defaults.overlayDisplay == 0);
    assert(defaults.telemetryDisplay == 0);
    assert(defaults.netImguiEnabled);
    assert(defaults.netImguiPort == 8888);

    const auto selected = parse({"probe", "--url", "rtsp://localhost/stream",
        "--raw-display", "3", "--overlay-display", "2",
        "--telemetry-display", "1", "--netimgui-port", "9999",
        "--disable-netimgui"});
    assert(selected.rawDisplay == 3);
    assert(selected.overlayDisplay == 2);
    assert(selected.telemetryDisplay == 1);
    assert(selected.netImguiPort == 9999);
    assert(!selected.netImguiEnabled);

    assert(fails({"probe", "--url", "rtsp://localhost", "--raw-display", "17"}));
    assert(fails({"probe", "--url", "rtsp://localhost", "--raw-display", "-1"}));
    assert(fails({"probe", "--url", "rtsp://localhost", "--netimgui-port", "0"}));
    assert(fails({"probe", "--url", "rtsp://localhost", "--netimgui-port", "65536"}));
}
