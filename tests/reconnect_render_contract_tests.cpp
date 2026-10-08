#include "media/IoDeadline.hpp"
#include "render/OverlayChainRecorder.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("reconnect/render contract test failed");
    }
}

struct OrderedOverlay final : reg::render::VideoOverlayRecorder {
    std::string& trace;
    char label;
    OrderedOverlay(std::string& value, char c)
        : trace(value), label(c) {}

    void record(
        VkCommandBuffer,
        VkFormat,
        VkExtent2D) override {
        trace.push_back(label);
    }
};

void testOverlayComposition() {
    std::string trace;
    OrderedOverlay identity{trace, 'I'};
    OrderedOverlay netImgui{trace, 'N'};
    reg::render::OverlayChainRecorder chain(&identity, &netImgui);
    chain.record(VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VkExtent2D{});
    require(trace == "IN");

    trace.clear();
    reg::render::OverlayChainRecorder onlyRemote(nullptr, &netImgui);
    onlyRemote.record(VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VkExtent2D{});
    require(trace == "N");

    trace.clear();
    reg::render::OverlayChainRecorder onlyLocal(&identity, nullptr);
    onlyLocal.record(VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VkExtent2D{});
    require(trace == "I");
}

void testIoDeadline() {
    using Clock = reg::media::IoDeadline::Clock;
    using namespace std::chrono;

    reg::media::IoDeadline deadline;
    const Clock::time_point start{seconds{1000}};
    require(!deadline.expired(start));

    deadline.armUntil(start + seconds{5});
    require(!deadline.expired(start + seconds{4}));
    require(deadline.expired(start + seconds{5}));
    require(deadline.expired(start + seconds{6}));

    // An EAGAIN from FFmpeg must NOT re-arm the deadline: silent UDP
    // reads must eventually trigger decoder reconnection.
    deadline.disarm();
    require(!deadline.expired(start + seconds{9}));

    deadline.armUntil(start + seconds{12});
    require(!deadline.expired(start + seconds{11}));
    require(deadline.expired(start + seconds{12}));
}

} // namespace

int main() {
    testOverlayComposition();
    testIoDeadline();
}
