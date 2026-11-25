#include "VtrFactory.hpp"
#include "vtr/VtrCatalogRead.inc"

namespace reg::vtr {
Catalog factoryCatalog() { return parseCatalog(std::string(kFactoryToml),"embedded-factory"); }
bool seedFactoryCatalog(const std::filesystem::path& path) {
    if(std::filesystem::exists(path)) return false;
    (void)factoryCatalog();
    if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto tmp=path;
    tmp+=".seed-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        out.write(kFactoryToml.data(),static_cast<std::streamsize>(kFactoryToml.size()));
        out.close();
        if(!out) throw std::runtime_error("Cannot write VTR factory seed");
        const bool created=std::filesystem::copy_file(tmp,path,std::filesystem::copy_options::skip_existing);
        std::filesystem::remove(tmp);
        return created;
    } catch(...) {
        std::error_code ignored;std::filesystem::remove(tmp,ignored);throw;
    }
}
namespace {
std::string toHex(Color color) {
    const auto byte=[](float v) { return static_cast<int>(std::lround(std::clamp(v,0.0F,1.0F)*255)); };
    std::ostringstream out;
    out<<'#'<<std::uppercase<<std::hex<<std::setfill('0')
       <<std::setw(2)<<byte(color.r)<<std::setw(2)<<byte(color.g)<<std::setw(2)<<byte(color.b);
    return out.str();
}
toml::table& ensureTable(toml::table& parent,const char* key) {
    if(!parent.contains(key)) {
        toml::table value;
        value.is_inline(true);
        parent.insert(key,std::move(value));
    }
    auto* table=parent.get_as<toml::table>(key);
    if(!table) throw std::runtime_error(std::string("expected table: ")+key);
    return *table;
}
void setBox(toml::table& t,const Box& b) {
    auto set=[&](const char* k,const auto& v) { t.insert_or_assign(k,v); };
    set("color",toHex(b.stroke.color));set("edges",b.stroke.pattern);
    set("edge_width",b.stroke.width);set("corners",b.corners);
    set("corner_width",b.cornerWidth);set("corner_px",b.cornerPx);
    set("diagonals",b.diagonals);set("diagonal_width",b.diagonalWidth);
    set("corner_layers",b.cornerLayers);set("frame_layers",b.frameLayers);
    set("layer_gap_px",b.layerGapPx);set("edge_markers",b.edgeMarkers);
    set("marker_width",b.markerWidth);set("marker_px",b.markerPx);
    set("rounding_px",b.roundingPx);set("outline_width",b.outlineWidth);
    set("outline_color",toHex(b.outlineColor));set("outline_alpha",b.outlineAlpha);
    set("fill",toHex(b.fill));set("fill_alpha",b.fillAlpha);
    set("dash_px",b.dashPx);set("gap_px",b.gapPx);set("dot_px",b.dotPx);
    set("adaptive",b.adaptive);set("edge_alpha",b.edgeAlpha);
    set("corner_alpha",b.cornerAlpha);set("marker_alpha",b.markerAlpha);
    set("corner_color",b.cornerColor?toHex(*b.cornerColor):std::string("inherit"));
    set("marker_color",b.markerColor?toHex(*b.markerColor):std::string("inherit"));
}
void setLabel(toml::table& t,const Label& b) {
    auto set=[&](const char* k,const auto& v) { t.insert_or_assign(k,v); };
    set("show",b.show);set("text",b.text);set("place",b.place);
    set("color",b.autoColor?std::string("auto"):toHex(b.color));
    set("background",b.boxBackground?std::string("box_color"):toHex(b.background));
    set("background_alpha",b.backgroundAlpha);set("font_px",b.fontPx);
    set("padding_px",b.paddingPx);set("gap_px",b.gapPx);
    set("rounding_px",b.roundingPx);set("max_width_px",b.maxWidthPx);
}
} // namespace
Catalog saveOutputStyle(const std::filesystem::path& path,std::uint64_t expectedHash,
                        const Output& style) {
    const auto current=readCatalog(path);
    if(current.hash!=expectedHash) throw std::runtime_error("TOML changed on disk; reload before saving");
    const auto match=std::find_if(current.outputs.begin(),current.outputs.end(),
        [&](const Output& o){return o.id==style.id&&o.key==style.key;});
    if(match==current.outputs.end()||match->geometry!=style.geometry)
        throw std::runtime_error("VTR output identity changed");
    auto document=toml::parse_file(path.string());
    auto* outputs=document.get_as<toml::table>("output");
    auto* entry=outputs?outputs->get_as<toml::table>(style.key):nullptr;
    if(!entry) throw std::runtime_error("VTR output is missing");
    entry->insert_or_assign("visible",style.visible);
    entry->insert_or_assign("z_order",style.zOrder);
    if(style.geometry=="bbox") {
        setBox(ensureTable(*entry,"box"),style.box);
        setLabel(ensureTable(*entry,"label"),style.label);
    } else {
        auto& osd=ensureTable(*entry,"osd");
        osd.insert_or_assign("color",toHex(style.osd.stroke.color));
        osd.insert_or_assign("width",style.osd.stroke.width);
        osd.insert_or_assign("pattern",style.osd.stroke.pattern);
        osd.insert_or_assign("alpha",style.osd.alpha);
        osd.insert_or_assign("outline_width",style.osd.outlineWidth);
        osd.insert_or_assign("outline_color",toHex(style.osd.outlineColor));
        osd.insert_or_assign("outline_alpha",style.osd.outlineAlpha);
        osd.insert_or_assign("dash_px",style.osd.dashPx);
        osd.insert_or_assign("gap_px",style.osd.gapPx);
        osd.insert_or_assign("dot_px",style.osd.dotPx);
        osd.insert_or_assign("fill",style.osd.fill?toHex(*style.osd.fill):std::string("inherit"));
        osd.insert_or_assign("fill_alpha",style.osd.fillAlpha);
        osd.insert_or_assign("point_radius_px",style.osd.pointRadiusPx);
        osd.insert_or_assign("arrow_head_px",style.osd.arrowHeadPx);
    }
    auto tmp=path;tmp+=L".tmp";
    auto backup=path;backup+=L".bak";
    try {
        {
            std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
            out<<document<<'\n';
            out.close();
            if(!out) throw std::runtime_error("Cannot write VTR temporary config");
        }
        auto verified=readCatalog(tmp);
        if(readCatalog(path).hash!=expectedHash)
            throw std::runtime_error("TOML changed during save; reload before saving");
        std::filesystem::copy_file(path,backup,std::filesystem::copy_options::overwrite_existing);
        std::filesystem::rename(tmp,path);
        return verified;
    } catch(...) {
        std::error_code ignored;
        std::filesystem::remove(tmp,ignored);
        throw;
    }
}

} // namespace reg::vtr
