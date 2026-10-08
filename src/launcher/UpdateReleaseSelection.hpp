#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace reg::launcher {

// GitHub does NOT guarantee that /releases is sorted by publication time.
// Compare actual published_at timestamps rather than list positions or SHA.
inline bool validReleaseTag(std::string_view tag, std::string_view channel) {
    if (channel == "dev") {
        if (tag.size() != 44 || !tag.starts_with("dev-")) return false;
        return std::all_of(tag.begin() + 4, tag.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    if (channel == "stable") {
        if (tag.size() < 6 || tag.size() > 40 || tag[0] != 'v') return false;
        return std::all_of(tag.begin() + 1, tag.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-';
        });
    }
    return false;
}

inline std::string releasePublishedAt(const nlohmann::json& release) {
    if (!release.is_object()) return {};
    const auto it = release.find("published_at");
    if (it == release.end() || !it->is_string()) return {};
    const auto& value = it->get_ref<const std::string&>();
    // GitHub emits timestamps in UTC as YYYY-MM-DDTHH:MM:SSZ.
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') return {};
    return value;
}

inline const nlohmann::json* latestRelease(
    const nlohmann::json& releases, std::string_view channel) {
    if (!releases.is_array()) return nullptr;
    const nlohmann::json* best = nullptr;
    std::string bestPublished;
    for (const auto& release : releases) {
        if (!release.is_object() ||
            release.value("draft", true) ||
            release.value("prerelease", channel == "stable") != (channel == "dev")) {
            continue;
        }
        const auto tag = release.value("tag_name", std::string{});
        const auto published = releasePublishedAt(release);
        if (!validReleaseTag(tag, channel) || published.empty()) continue;
        if (!best || published > bestPublished) {
            best = &release;
            bestPublished = published;
        }
    }
    return best;
}

inline const nlohmann::json* releaseWithTag(
    const nlohmann::json& releases, std::string_view tag) {
    if (!releases.is_array()) return nullptr;
    for (const auto& release : releases) {
        if (release.is_object() && release.value("tag_name", std::string{}) == tag) {
            return &release;
        }
    }
    return nullptr;
}

} // namespace reg::launcher
