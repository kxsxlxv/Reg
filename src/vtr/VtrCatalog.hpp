#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace reg::vtr {

// Public, renderer-independent description shared by VTR senders and REG.
// The file is a catalogue of output channels, NOT an algorithm configuration.
struct Color {
    float r{1.0F}, g{1.0F}, b{1.0F};
};
struct Stroke {
    Color color{};
    float width{2.0F};
    std::string pattern{"solid"}; // none, solid, dashed, dash_dot
};
struct Box {
    Stroke stroke{};
    std::string corners{"none"};
    std::string diagonals{"none"};
    float cornerPx{16.0F};
    float cornerWidth{2.0F};
    float diagonalWidth{2.0F};
    int cornerLayers{1};
    int frameLayers{1};
    float layerGapPx{4.0F};
    std::string edgeMarkers{"none"}; // none, center, outside
    float markerWidth{3.0F};
    float markerPx{12.0F};
    float roundingPx{0.0F};
    float outlineWidth{0.0F}; // extra border on each side
    Color outlineColor{12.0F/255.0F,16.0F/255.0F,20.0F/255.0F};
    float outlineAlpha{1.0F};
    // Zero selects the legacy pattern-specific default.
    float dashPx{0},gapPx{0},dotPx{0};
    bool adaptive{false};
    float edgeAlpha{1},cornerAlpha{1},markerAlpha{1};
    std::optional<Color> cornerColor,markerColor;
    Color fill{};
    float fillAlpha{0.0F};
};
struct Label {
    bool show{false};
    std::string text;
    std::string place{"top_left_outside"};
    Color color{};
    Color background{0.0F, 0.0F, 0.0F};
    float backgroundAlpha{0.70F};
    float fontPx{14.0F};
    float paddingPx{4.0F};
    float gapPx{3.0F};
    float roundingPx{3.0F};
    float maxWidthPx{0}; // 0 = unconstrained, except viewport
    bool autoColor{false};
    bool boxBackground{false};
};
struct Osd {
    Stroke stroke{};
    float alpha{1.0F};
};
struct Output {
    std::uint16_t id{};
    std::string key;
    std::string semantic;
    std::string geometry;
    std::string benchmark{"none"};
    bool visible{true};
    int zOrder{0};
    Box box{};
    Label label{};
    Osd osd{};
};
struct Catalog {
    std::uint64_t hash{}; // FNV-1a 64 of exact TOML bytes; NOT a security checksum
    std::vector<Output> outputs;
};

// Throws on invalid TOML, unknown geometry, duplicate IDs or unsupported format.
// Consumers should keep the last valid snapshot on a failed manual reload.
Catalog readCatalog(const std::filesystem::path& path);
// Preserve unknown TOML values and output identity; refuse stale edits.
// Writes validated temporary file, keeps original as .bak, then replaces.
Catalog saveOutputStyle(const std::filesystem::path& path,std::uint64_t expectedHash,
                        const Output& style);
std::string hashHex(std::uint64_t hash);
Color contrastingText(Color background); // choose black/white by WCAG relative luminance
} // namespace reg::vtr
