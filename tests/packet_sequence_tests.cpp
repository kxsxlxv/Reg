#include "metadata/PacketSequenceTracker.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void inOrderAndGapAreClassified() {
    reg::metadata::PacketSequenceTracker tracker;

    require(
        tracker.observe(100).disposition == reg::metadata::SequenceDisposition::First,
        "first packet classification mismatch");

    require(
        tracker.observe(101).disposition == reg::metadata::SequenceDisposition::InOrder,
        "in-order packet classification mismatch");

    const auto gap = tracker.observe(105);
    require(
        gap.disposition == reg::metadata::SequenceDisposition::Gap,
        "gap classification mismatch");
    require(gap.missingBeforeThisPacket == 3, "gap size mismatch");
}

void latePacketAndDuplicateAreDistinguished() {
    reg::metadata::PacketSequenceTracker tracker;
    (void)tracker.observe(100);
    (void)tracker.observe(103);

    const auto late = tracker.observe(102);
    require(
        late.disposition == reg::metadata::SequenceDisposition::OutOfOrder,
        "late packet classification mismatch");

    const auto duplicateLate = tracker.observe(102);
    require(
        duplicateLate.disposition == reg::metadata::SequenceDisposition::Duplicate,
        "duplicate late packet classification mismatch");

    const auto duplicateHighest = tracker.observe(103);
    require(
        duplicateHighest.disposition == reg::metadata::SequenceDisposition::Duplicate,
        "duplicate highest packet classification mismatch");
}

void wrapAroundIsForwardProgress() {
    reg::metadata::PacketSequenceTracker tracker;
    const auto max = std::numeric_limits<std::uint32_t>::max();

    (void)tracker.observe(max - 1U);
    require(
        tracker.observe(max).disposition == reg::metadata::SequenceDisposition::InOrder,
        "max uint32 packet was not in order");
    require(
        tracker.observe(0).disposition == reg::metadata::SequenceDisposition::InOrder,
        "uint32 wrap was not treated as forward progress");
    require(
        tracker.observe(1).disposition == reg::metadata::SequenceDisposition::InOrder,
        "post-wrap packet was not in order");
}

void packetsOutsideWindowBecomeStale() {
    reg::metadata::PacketSequenceTracker tracker;
    (void)tracker.observe(100);
    (void)tracker.observe(200);

    require(
        tracker.observe(100).disposition == reg::metadata::SequenceDisposition::Stale,
        "packet outside duplicate window was not stale");
}

void resetStartsNewSequenceObservation() {
    reg::metadata::PacketSequenceTracker tracker;
    (void)tracker.observe(100);
    tracker.reset();

    require(!tracker.initialized(), "reset did not clear initialized state");
    require(
        tracker.observe(4).disposition == reg::metadata::SequenceDisposition::First,
        "first packet after reset was not classified as first");
}

} // namespace

int main() {
    try {
        inOrderAndGapAreClassified();
        latePacketAndDuplicateAreDistinguished();
        wrapAroundIsForwardProgress();
        packetsOutsideWindowBecomeStale();
        resetStartsNewSequenceObservation();

        std::cout << "packet_sequence_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "packet_sequence_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
