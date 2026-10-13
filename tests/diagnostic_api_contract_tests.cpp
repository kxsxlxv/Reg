#include "diagnostics/api/DiagnosticContract.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

int main() {
    using namespace reg::diagnostics::api;
    static_assert(std::is_copy_constructible_v<Snapshot>);
    static_assert(std::is_copy_constructible_v<HttpResponse>);

    Snapshot live;
    assert(live.schemaVersion == "1.0");
    assert(live.hostRole == "reg");
    assert(live.dataOrigin == "live");
    assert(live.sections.empty()); // empty is NOT evidence of health

    Section video;
    video.name = "video";
    video.availability = Availability::Unavailable;
    video.statuses.push_back(Status{});
    assert(!video.statuses.back().booleanValue.has_value());

    Metric counter;
    counter.name = "decoder.decoded_frames_total";
    counter.kind = MetricKind::Counter;
    counter.counter = std::numeric_limits<std::uint64_t>::max();
    counter.availability = Availability::Fresh;
    assert(*counter.counter == std::numeric_limits<std::uint64_t>::max());
    assert(!counter.value.has_value()); // integer counter != floating-point gauge
    video.metrics.push_back(counter);
    live.sections.push_back(video);

    const Snapshot immutableCopy = live;
    live.sections.front().metrics.front().counter = 1U;
    assert(immutableCopy.sections.front().metrics.front().counter.value() ==
           std::numeric_limits<std::uint64_t>::max());

    HttpRequest request{"GET", "/healthz"};
    assert(request.method == "GET");
    HttpResponse response{};
    assert(response.statusCode == 200);
    assert(response.body.empty());
}
