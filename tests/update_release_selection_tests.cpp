#include "launcher/UpdateReleaseSelection.hpp"

#include <nlohmann/json.hpp>
#include <cassert>
#include <string>

namespace {
using nlohmann::json;

std::string tag(char hex) {
    return "dev-" + std::string(40, hex);
}
json dev(char hex, const char* when, bool draft = false) {
    return {
        {"tag_name", tag(hex)}, {"published_at", when},
        {"draft", draft}, {"prerelease", true}
    };
}
}

int main() {
    using reg::launcher::latestRelease;
    using reg::launcher::releasePublishedAt;
    using reg::launcher::releaseWithTag;
    using reg::launcher::validReleaseTag;

    // Reproduces the real-world GitHub response: the first item was d7f,
    // while the newest publish time belonged to 543.
    const json releases = json::array({
        dev('d', "2026-10-08T12:46:50Z"),
        dev('b', "2026-10-08T13:00:54Z"),
        dev('a', "2026-10-08T12:34:24Z"),
        dev('5', "2026-10-08T13:11:38Z"),
        dev('f', "2026-10-09T13:11:38Z", true),
        {{"tag_name", "v1.0.0"}, {"prerelease", false},
         {"draft", false}, {"published_at", "2026-10-08T13:15:00Z"}}
    });
    const auto* newest = latestRelease(releases, "dev");
    assert(newest != nullptr);
    assert(newest->at("tag_name") == tag('5'));
    assert(newest != &releases[0]);
    assert(releaseWithTag(releases, tag('b')) == &releases[1]);
    assert(releasePublishedAt(*newest) > releasePublishedAt(releases[1]));

    const auto* stable = latestRelease(releases, "stable");
    assert(stable && stable->at("tag_name") == "v1.0.0");
    assert(latestRelease(json::array({dev('a', "2026-10-08T12:34:24Z", true)}), "dev")
           == nullptr);
    assert(latestRelease(json::object(), "dev") == nullptr);

    assert(validReleaseTag(tag('a'), "dev"));
    assert(!validReleaseTag("dev-not-a-sha", "dev"));
    assert(!validReleaseTag(tag('g'), "dev"));
    assert(!validReleaseTag("v1.0.0", "dev"));

    // Missing/invalid published_at never silently wins a release selection.
    json corrupt = dev('c', "2026-10-11T20:00:00Z");
    corrupt["published_at"] = nullptr;
    json incomplete = json::array({corrupt, dev('a', "2026-10-08T12:00:00Z")});
    const auto* candidate = latestRelease(incomplete, "dev");
    assert(candidate && candidate->at("tag_name") == tag('a'));
    assert(releasePublishedAt(corrupt).empty());
    assert(releaseWithTag(incomplete, tag('c')) == &incomplete[0]);
}
