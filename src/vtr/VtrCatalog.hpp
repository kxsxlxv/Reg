#pragma once
#include <cstdint>
#include <filesystem>
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
    std::string pattern{"solid"}; // none, solid, dashed
};
struct Box {
    Stroke stroke{};
    std::string corners{"none"};
    std::string diagonals{"none"};
    float cornerPx{16.0F};
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
std::string hashHex(std::uint64_t hash);
} // namespace reg::vtr
