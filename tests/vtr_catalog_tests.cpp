#include "vtr/VtrCatalog.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition,const char* msg) {
    if (!condition) throw std::runtime_error(msg);
}
void write(const std::filesystem::path& p,const char* text) {
    std::ofstream out(p,std::ios::binary|std::ios::trunc);
    out << text;
}
}

int main() {
    using namespace reg::vtr;
    const auto config=std::filesystem::path("config")/"vtr.toml";
    require(std::filesystem::exists(config),"Missing deployed VTR config");
    const auto catalog=readCatalog(config);
    require(catalog.outputs.size()>=14U,"Expected real GT/CV output catalogue");
    require(catalog.hash!=0U,"Expected nonzero hash");
    bool sawLinear=false,sawMot=false;
    for(const auto& output:catalog.outputs) {
        if(output.key=="linear_propagation") {
            require(output.geometry=="bbox" && output.benchmark=="none" &&
                    output.box.stroke.pattern=="none" &&
                    output.box.fillAlpha>0.0F && !output.label.show,
                    "Linear OSD must be fill-only/operator-only");
            sawLinear=true;
        }
        if(output.key=="mot_track") {
            require(output.box.corners=="solid" && output.label.show,
                    "MOT styled corners and label missing");
            sawMot=true;
        }
    }
    require(sawLinear&&sawMot,"Required output types missing");
    for(const auto& o:catalog.outputs) {
        if(o.key=="neural_detection")
            require(o.label.autoColor&&o.label.boxBackground&&o.label.gapPx==0&&
                o.label.backgroundAlpha==1&&o.label.roundingPx==0,"Ultralytics-style defaults missing");
        if(o.key=="mot_prediction")
            require(o.box.stroke.pattern=="dashed"&&o.box.diagonals=="none","Prediction must not be crossed out");
        if(o.key=="mot_ofa")
            require(o.box.stroke.pattern=="dash_dot","OFA dash-dot missing");
        if(o.key=="sot_track")
            require(o.box.stroke.pattern=="dashed"&&o.box.cornerWidth>o.box.stroke.width,
                "SOT must preserve independently thick corners and dashed edges");
    }
    require(contrastingText({1,1,1}).r==0&&contrastingText({0,0,0}).r==1,
            "Automatic label contrast failed");
    const auto tmp=std::filesystem::path("vtr-test-invalid.toml");
    write(tmp,"format = 1\n[output.a]\nid = 1\nsemantic = 'detection'\ngeometry = 'bbox'\n"
              "[output.b]\nid = 1\nsemantic = 'detection'\ngeometry = 'bbox'\n");
    bool rejected=false;
    try { (void)readCatalog(tmp); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Duplicate output ID was accepted");
    write(tmp,"format = 99\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\n");
    rejected=false;
    try { (void)readCatalog(tmp); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Unsupported format was accepted");
    write(tmp,"format = 1\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\n");
    const auto hash1=readCatalog(tmp).hash;
    write(tmp,"format = 1\n# changed\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\n");
    require(readCatalog(tmp).hash!=hash1,"Hash did not change");
    write(tmp,"format=1\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\n"
              "box={width=3,edge_width=1,corners='solid',corner_layers=2,frame_layers=2,"
              "edge_markers='outside',rounding_px=5,outline_width=1}\n"
              "label={color='#FFFFFF',background='#000000',gap_px=3}\n");
    const auto styled=readCatalog(tmp).outputs.at(0);
    require(styled.box.stroke.width==1&&styled.box.cornerWidth==3&&
        styled.box.cornerLayers==2&&styled.box.frameLayers==2&&
        styled.box.edgeMarkers=="outside"&&styled.box.roundingPx==5&&
        styled.box.outlineWidth==1&&!styled.label.autoColor&&!styled.label.boxBackground,
        "Composable styles or legacy color compatibility failed");
    write(tmp,"format=1\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\nbox={corner_layers=3}\n");
    rejected=false;
    try { (void)readCatalog(tmp); } catch(const std::exception& ex) {
        rejected=std::string(ex.what()).find("expected 1 or 2: corner_layers")!=std::string::npos;
    }
    require(rejected,"Invalid corner_layers must be rejected by the catalogue validator");
    require(styled.box.outlineAlpha==1.0F&&
            styled.box.outlineColor.r==12.0F/255.0F&&
            styled.box.outlineColor.g==16.0F/255.0F&&
            styled.box.outlineColor.b==20.0F/255.0F,
            "Legacy outline defaults changed");
    write(tmp,"format=1\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\n"
              "box={outline_width=2,outline_color='#FF0080',outline_alpha=0.5}\n");
    const auto outline=readCatalog(tmp).outputs.at(0).box;
    require(outline.outlineWidth==2&&outline.outlineAlpha==0.5F&&
        outline.outlineColor.r==1&&outline.outlineColor.g==0&&
        outline.outlineColor.b==128.0F/255.0F,"Outline style parse failed");
    for(const char* value:{"outline_alpha=1.01","outline_alpha=-0.1","outline_color='black'"}) {
        const std::string fixture="format=1\n[output.a]\nid=1\nsemantic='detection'\ngeometry='bbox'\nbox={"+
            std::string(value)+"}\n";
        write(tmp,fixture.c_str());
        rejected=false;
        try { (void)readCatalog(tmp); } catch(const std::exception&) { rejected=true; }
        require(rejected,"Invalid outline style accepted");
    }
    std::filesystem::remove(tmp);
    std::cout<<"VTR TOML catalogue: PASS\n";
}
