#include "render/Cvm1SemanticStyleConfig.hpp"
#include "render/Cvm1SemanticStyle.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace reg::render::cvm1 {
namespace {
using Clock = std::chrono::steady_clock;
std::filesystem::path pathOfConfig() {
    std::error_code ec;
    const auto local = std::filesystem::current_path(ec) / "config/gt-range-ui.json";
    if (!ec && std::filesystem::exists(local, ec)) return local;
    if (const char* exe = SDL_GetBasePath(); exe != nullptr)
        return std::filesystem::path(exe) / "config/gt-range-ui.json";
    return local;
}
Rgba colorFromHex(const std::string& s, Rgba fallback) {
    if (s.size() != 7 || s[0] != '#') return fallback;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    unsigned val = 0;
    for (int i = 1; i < 7; ++i) {
        const int n = hex(s[static_cast<std::size_t>(i)]);
        if (n < 0) return fallback;
        val = (val << 4U) | static_cast<unsigned>(n);
    }
    return fromRgb(val);
}
void parseStyles(const nlohmann::json& j, Palette& palette) {
    if (j.contains("styles") && j.at("styles").is_object()) {
        const auto& styles = j.at("styles");
        for (std::size_t i = 0; i < kStyleCount; ++i) {
            const auto it = styles.find(kStyleKeys[i]);
            if (it == styles.end() || !it->is_object()) continue;
            auto& dst = palette.styles[i];
            dst.color = colorFromHex(it->value("color", std::string("#AAB2BD")), dst.color);
            dst.width = std::clamp(it->value("width", dst.width), 0.5F, 12.0F);
            dst.markerSize = std::clamp(it->value("marker_size", dst.markerSize), 2.0F, 16.0F);
            dst.label = it->value("label", dst.label);
            if (dst.label.size() > 64U) dst.label.resize(64U);
            dst.visible = it->value("visible", dst.visible);
            const std::string pattern = it->value("pattern", std::string("solid"));
            if (pattern == "solid") dst.pattern = Pattern::solid;
            else if (pattern == "dashed") dst.pattern = Pattern::dashed;
            else if (pattern == "corners") dst.pattern = Pattern::corners;
            else throw std::runtime_error("unsupported pattern: " + pattern);
        }
    } else if (j.contains("cv_markers") && j.at("cv_markers").is_array()) {
        // Version 1 compatibility: project its legacy rules into shared styles.
        for (const auto& item : j.at("cv_markers")) {
            if (!item.is_object()) continue;
            const unsigned mask = item.value("flags_mask", 0U);
            const unsigned value = item.value("flags_value", 0U);
            if (mask > 0xffffU || value > 0xffffU) continue;
            const Kind kind = mask == kDetector ? Kind::detection
                : (mask == kPropagated ? Kind::propagated : Kind::fallback);
            auto& dst = palette.styles[Palette::index(kind)];
            dst.color = colorFromHex(
                item.value("color", std::string("#AAB2BD")), dst.color);
            dst.markerSize = std::clamp(
                item.value("marker_size", dst.markerSize), 2.0F, 16.0F);
            dst.label = item.value("label", dst.label);
        }
    }
    if (j.contains("rules") && j.at("rules").is_array()) {
        for (const auto& rule : j.at("rules")) {
            if (!rule.is_object() || palette.rules.size() >= 64U) continue;
            const unsigned mask = rule.value("flags_mask", 0U);
            const unsigned value = rule.value("flags_value", 0U);
            if (mask == 0U || mask > kKnownMask || value > kKnownMask ||
                (value & ~mask) != 0U) continue;
            const std::string key = rule.value("style", std::string("fallback"));
            for (std::size_t i = 0; i < kStyleCount; ++i) {
                if (key == kStyleKeys[i]) {
                    palette.rules.push_back(Rule{
                        static_cast<std::uint16_t>(mask),
                        static_cast<std::uint16_t>(value),
                        static_cast<Kind>(i)
                    });
                    break;
                }
            }
        }
    }
}
} // namespace
void refreshConfig() {
    static Clock::time_point nextCheck{};
    static std::filesystem::path lastPath;
    static std::filesystem::file_time_type lastWrite{};
    static bool initialized{};
    const auto now = Clock::now();
    if (now < nextCheck) return;
    nextCheck = now + std::chrono::milliseconds(300);
    const auto path = pathOfConfig();
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || (initialized && path == lastPath && stamp == lastWrite))
        return;
    // Cache a bad config's timestamp too: keep the previously valid palette
    // and log only once until the file changes.
    initialized = true;
    lastPath = path;
    lastWrite = stamp;
    try {
        std::ifstream stream(path);
        const auto json = nlohmann::json::parse(stream);
        auto palette = std::make_shared<Palette>();
        parseStyles(json, *palette);
        publish(std::move(palette));
    } catch (const std::exception& error) {
        std::cerr << "[cvm1-style] invalid " << path.string() << ": "
                  << error.what() << '\n';
    }
}
} // namespace reg::render::cvm1
