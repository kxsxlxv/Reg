// Shared Reg and Jetson read-side catalogue and byte-identity policy.
#include "vtr/VtrCatalog.hpp"
#include <chrono>
#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#else
#include <toml.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace reg::vtr {
namespace {
std::string str(const toml::table& t, const char* name, std::string fallback = {}) {
    if (const auto value = t[name].value<std::string>())
        return *value;
    return fallback;
}
float number(const toml::table& t, const char* name, float fallback) {
    const toml::node* node = t.get(name);
    if (!node) return fallback;
    double x = 0.0;
    if (auto* f = node->as_floating_point()) x = f->get();
    else if (auto* i = node->as_integer()) x = static_cast<double>(i->get());
    else throw std::runtime_error(std::string("expected a number: ") + name);
    if (!std::isfinite(x) || x < -1.0e6 || x > 1.0e6)
        throw std::runtime_error(std::string("number out of range: ") + name);
    return static_cast<float>(x);
}
float bounded(const toml::table& t, const char* name, float fallback,
              float lo, float hi) {
    float x = number(t, name, fallback);
    if (x < lo || x > hi)
        throw std::runtime_error(std::string("out of range: ") + name);
    return x;
}
Color hex(const std::string& value) {
    if (value.size() != 7U || value[0] != '#')
        throw std::runtime_error("VTR color must be #RRGGBB: " + value);
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c-'0';
        if (c >= 'a' && c <= 'f') return c-'a'+10;
        if (c >= 'A' && c <= 'F') return c-'A'+10;
        return -1;
    };
    int byte[3]{};
    for (int n=0;n<3;++n) {
        const int a = digit(value[1+n*2]), b = digit(value[2+n*2]);
        if (a < 0 || b < 0) throw std::runtime_error("invalid hex color");
        byte[n] = a * 16 + b;
    }
    return {byte[0]/255.0F, byte[1]/255.0F, byte[2]/255.0F};
}
Color color(const toml::table& t, const char* key, Color fallback) {
    const toml::node* node = t.get(key);
    if (!node) return fallback;
    auto value = t[key].value<std::string>();
    if (!value) throw std::runtime_error(std::string("expected color: ")+key);
    return hex(*value);
}
std::string pattern(const toml::table& t, const char* key,
                    std::string fallback) {
    const std::string result = str(t, key, std::move(fallback));
    if (result != "none" && result != "solid" && result != "dashed" && result != "dash_dot")
        throw std::runtime_error(std::string("unknown line pattern: ")+result);
    return result;
}
void readBox(const toml::table& t, Box& out) {
    out.stroke.color = color(t, "color", out.stroke.color);
    // Legacy width remains the default for all stroke components.
    const float legacyWidth = bounded(t, "width", out.stroke.width, 0.5F, 16.0F);
    out.stroke.width = bounded(t, "edge_width", legacyWidth, 0.5F, 16.0F);
    out.cornerWidth = bounded(t, "corner_width", legacyWidth, 0.5F, 16.0F);
    out.diagonalWidth = bounded(t, "diagonal_width", legacyWidth, 0.5F, 16.0F);
    out.stroke.pattern = pattern(t, "edges", "none");
    const auto layers = [&](const char* key) {
        const auto* node = t.get(key);
        if (!node) return 1;
        const auto value = t[key].value<std::int64_t>();
        if (!value || (*value != 1 && *value != 2))
            throw std::runtime_error(std::string("expected 1 or 2: ") + key);
        return static_cast<int>(*value);
    };
    out.cornerLayers = layers("corner_layers");
    out.frameLayers = layers("frame_layers");
    out.layerGapPx = bounded(t, "layer_gap_px", out.layerGapPx, 1.0F, 24.0F);
    out.edgeMarkers = str(t, "edge_markers", "none");
    if (out.edgeMarkers != "none" && out.edgeMarkers != "center" &&
        out.edgeMarkers != "outside")
        throw std::runtime_error("unknown edge_markers style");
    out.markerWidth = bounded(t, "marker_width", out.markerWidth, 0.5F, 16.0F);
    out.markerPx = bounded(t, "marker_px", out.markerPx, 1.0F, 200.0F);
    out.roundingPx = bounded(t, "rounding_px", out.roundingPx, 0.0F, 100.0F);
    out.outlineWidth = bounded(t, "outline_width", out.outlineWidth, 0.0F, 8.0F);
    out.outlineColor = color(t, "outline_color", out.outlineColor);
    out.outlineAlpha = bounded(t, "outline_alpha", out.outlineAlpha, 0.0F, 1.0F);
    out.dashPx = bounded(t,"dash_px",out.dashPx,0,64);
    out.gapPx = bounded(t,"gap_px",out.gapPx,0,32);
    out.dotPx = bounded(t,"dot_px",out.dotPx,0,16);
    if(t.contains("adaptive")) {
        const auto v=t["adaptive"].value<bool>();
        if(!v) throw std::runtime_error("expected bool: adaptive");
        out.adaptive=*v;
    }
    out.edgeAlpha=bounded(t,"edge_alpha",out.edgeAlpha,0,1);
    out.cornerAlpha=bounded(t,"corner_alpha",out.cornerAlpha,0,1);
    out.markerAlpha=bounded(t,"marker_alpha",out.markerAlpha,0,1);
    if(t.contains("corner_color")) {
        if(str(t,"corner_color")=="inherit") out.cornerColor.reset();
        else out.cornerColor=color(t,"corner_color",out.stroke.color);
    }
    if(t.contains("marker_color")) {
        if(str(t,"marker_color")=="inherit") out.markerColor.reset();
        else out.markerColor=color(t,"marker_color",out.stroke.color);
    }
    out.corners = pattern(t, "corners", "none");
    out.diagonals = pattern(t, "diagonals", "none");
    out.cornerPx = bounded(t, "corner_px", out.cornerPx, 1.0F, 200.0F);
    out.fill = color(t, "fill", out.stroke.color);
    out.fillAlpha = bounded(t, "fill_alpha", 0.0F, 0.0F, 1.0F);
}
void readLabel(const toml::table& t, Label& out, bool isOutput) {
    out.show = t["show"].value<bool>().value_or(isOutput ? true : out.show);
    out.text = str(t, "text", out.text);
    out.place = str(t, "place", out.place);
    if (out.place != "top_left_outside" && out.place != "bottom_left_outside")
        throw std::runtime_error("unsupported VTR label placement");
    if (t.contains("color")) {
        const auto value = t["color"].value<std::string>();
        if (!value) throw std::runtime_error("expected label color");
        out.autoColor = *value == "auto";
        if (!out.autoColor) out.color = hex(*value);
    }
    if (t.contains("background")) {
        const auto value = t["background"].value<std::string>();
        if (!value) throw std::runtime_error("expected label background");
        out.boxBackground = *value == "box_color";
        if (!out.boxBackground) out.background = hex(*value);
    }
    out.backgroundAlpha = bounded(t, "background_alpha", out.backgroundAlpha, 0.0F, 1.0F);
    out.fontPx = bounded(t, "font_px", out.fontPx, 8.0F, 48.0F);
    out.paddingPx = bounded(t, "padding_px", out.paddingPx, 0.0F, 24.0F);
    out.gapPx = bounded(t, "gap_px", out.gapPx, 0.0F, 24.0F);
    out.roundingPx = bounded(t, "rounding_px", out.roundingPx, 0.0F, 24.0F);
    out.maxWidthPx = bounded(t,"max_width_px",out.maxWidthPx,0,2048);
    if (out.text.size() > 128U) throw std::runtime_error("VTR label template too long");
}
void readOsd(const toml::table& t, Osd& out) {
    out.stroke.color = color(t, "color", out.stroke.color);
    out.stroke.width = bounded(t, "width", out.stroke.width, 0.5F, 16.0F);
    out.stroke.pattern = pattern(t, "pattern", "solid");
    out.alpha = bounded(t, "alpha", out.alpha, 0.0F, 1.0F);
    out.outlineWidth=bounded(t,"outline_width",out.outlineWidth,0,8);
    out.outlineColor=color(t,"outline_color",out.outlineColor);
    out.outlineAlpha=bounded(t,"outline_alpha",out.outlineAlpha,0,1);
    out.dashPx=bounded(t,"dash_px",out.dashPx,0,64);
    out.gapPx=bounded(t,"gap_px",out.gapPx,0,32);
    out.dotPx=bounded(t,"dot_px",out.dotPx,0,16);
    if(t.contains("fill")) {
        if(str(t,"fill")=="inherit") out.fill.reset();
        else out.fill=color(t,"fill",out.stroke.color);
    }
    out.fillAlpha=bounded(t,"fill_alpha",out.fillAlpha,0,1);
    out.pointRadiusPx=bounded(t,"point_radius_px",out.pointRadiusPx,1,64);
    out.arrowHeadPx=bounded(t,"arrow_head_px",out.arrowHeadPx,2,64);
}
bool supported(std::string_view s) {
    constexpr std::array<std::string_view, 11> supportedTypes{
        "bbox","line","polyline","polygon","circle","ellipse",
        "point","arrow","text","mask","heatmap"};
    return std::find(supportedTypes.begin(), supportedTypes.end(), s)
        != supportedTypes.end();
}
} // namespace

static Catalog parseCatalog(const std::string& bytes,const std::filesystem::path& path) {
    if (bytes.size() > 256U * 1024U)
        throw std::runtime_error("VTR catalogue exceeds 256 KiB");
    const toml::table document = toml::parse(bytes, path.string());
    const auto format = document["format"].value<std::int64_t>();
    if (!format || *format != 1)
        throw std::runtime_error("Unsupported VTR TOML format (expected format = 1)");
    const auto* outputs = document.get_as<toml::table>("output");
    if (!outputs || outputs->empty() || outputs->size() > 128U)
        throw std::runtime_error("VTR requires 1..128 [output.*] sections");

    Catalog result;
    result.hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) {
        result.hash ^= byte;
        result.hash *= UINT64_C(1099511628211);
    }
    Label defaults;
    if (const auto* d = document.get_as<toml::table>("defaults"))
        if (const auto* l = d->get_as<toml::table>("label"))
            readLabel(*l, defaults, false);

    std::set<std::uint16_t> ids;
    for (const auto& [name, node] : *outputs) {
        const auto* entry = node.as_table();
        if (!entry) throw std::runtime_error("VTR output must be a table");
        const auto id = (*entry)["id"].value<std::int64_t>();
        if (!id || *id < 1 || *id > 65535)
            throw std::runtime_error("VTR output id must be uint16 > 0");
        const auto narrowed = static_cast<std::uint16_t>(*id);
        if (!ids.insert(narrowed).second)
            throw std::runtime_error("Duplicate VTR output id");
        Output output;
        output.id = narrowed;
        output.key = std::string(name.str());
        output.semantic = str(*entry, "semantic");
        output.geometry = str(*entry, "geometry");
        output.benchmark = str(*entry, "benchmark", "none");
        if(entry->contains("visible")) {
            const auto v=(*entry)["visible"].value<bool>();
            if(!v) throw std::runtime_error("expected bool: visible");
            output.visible=*v;
        }
        if(entry->contains("z_order")) {
            const auto z=(*entry)["z_order"].value<std::int64_t>();
            if(!z||*z< -1000||*z>1000) throw std::runtime_error("invalid z_order");
            output.zOrder=static_cast<int>(*z);
        }
        if (output.semantic.empty() || !supported(output.geometry))
            throw std::runtime_error("Missing semantic or unknown geometry for " + output.key);
        output.label = defaults;
        if (const auto* box = entry->get_as<toml::table>("box"))
            readBox(*box, output.box);
        if (const auto* label = entry->get_as<toml::table>("label"))
            readLabel(*label, output.label, true);
        if (const auto* osd = entry->get_as<toml::table>("osd"))
            readOsd(*osd, output.osd);
        result.outputs.push_back(std::move(output));
    }
    std::sort(result.outputs.begin(), result.outputs.end(),
              [](const Output& a, const Output& b) { return a.id < b.id; });
    return result;
}

Catalog readCatalog(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("VTR config not found: "+path.string());
    const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    return parseCatalog(bytes,path);
}
Color contrastingText(Color background) {
    const auto linear = [](float x) {
        return x <= 0.04045F ? x / 12.92F : std::pow((x+0.055F)/1.055F,2.4F);
    };
    const float lum = 0.2126F*linear(background.r) +
        0.7152F*linear(background.g) + 0.0722F*linear(background.b);
    // Compare WCAG contrast ratios against black and white.
    return (lum+0.05F)/0.05F >= 1.05F/(lum+0.05F)
        ? Color{0,0,0} : Color{1,1,1};
}

std::string hashHex(std::uint64_t hash) {
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}
} // namespace reg::vtr
