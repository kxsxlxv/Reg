#include "vtr/VtrCatalog.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
    write(tmp,"format=1\\n[output.a]\\nid=1\\nsemantic='detection'\\ngeometry='bbox'\\n"
              "box={width=3,edge_width=1,corners='solid',corner_layers=2,frame_layers=2,"
              "edge_markers='outside',rounding_px=5,outline_width=1}\\n"
              "label={color='#FFFFFF',background='#000000',gap_px=3}\\n");
    const auto styled=readCatalog(tmp).outputs.at(0);
    require(styled.box.stroke.width==1&&styled.box.cornerWidth==3&&
        styled.box.cornerLayers==2&&styled.box.frameLayers==2&&
        styled.box.edgeMarkers=="outside"&&styled.box.roundingPx==5&&
        styled.box.outlineWidth==1&&!styled.label.autoColor&&!styled.label.boxBackground,
        "Composable styles or legacy color compatibility failed");
    write(tmp,"format=1\\n[output.a]\\nid=1\\nsemantic='detection'\\ngeometry='bbox'\\nbox={corner_layers=3}\\n");
    rejected=false;
    try { (void)readCatalog(tmp); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Unsupported corner layer count accepted");
    std::filesystem::remove(tmp);
    std::cout<<"VTR TOML catalogue: PASS\n";
}
