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
    std::filesystem::remove(tmp);
    std::cout<<"VTR TOML catalogue: PASS\n";
}
