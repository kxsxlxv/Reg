#include "vtr/VtrBoxGeometry.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace reg::vtr;
using namespace reg::vtr::boxgeom;
void require(bool x,const char* message) { if(!x) throw std::runtime_error(message); }
bool near(Point a,Point b) { return distance(a,b)<0.025F; }
// Each segment, including the closing segment, must have a reflected
// equivalent with the same stroke width. Compare independent of direction.
struct Segment { Point a,b; float width; };
std::vector<Segment> segments(const Lines& lines) {
    std::vector<Segment> result;
    for(const auto& line:lines) {
        require(line.points.size()>=2,"empty stroke");
        for(std::size_t i=1;i<line.points.size();++i)
            result.push_back({line.points[i-1],line.points[i],line.width});
        if(line.closed) result.push_back({line.points.back(),line.points.front(),line.width});
    }
    return result;
}
int main() {
    try {
        Box base; base.stroke.width=2; base.cornerWidth=4; base.cornerPx=17;
        std::vector<Box> styles;
        Box s=base; styles.push_back(s);
        s=base;s.stroke.pattern="dashed";styles.push_back(s);
        s=base;s.stroke.pattern="dash_dot";styles.push_back(s);
        s=base;s.stroke.pattern="none";s.corners="solid";styles.push_back(s);
        s=base;s.stroke.pattern="dashed";s.corners="solid";styles.push_back(s);
        s=base;s.stroke.width=1;s.corners="solid";styles.push_back(s);
        s=base;s.stroke.pattern="none";s.corners="solid";s.cornerLayers=2;s.cornerWidth=2;styles.push_back(s);
        s=base;s.edgeMarkers="outside";styles.push_back(s);
        s=base;s.edgeMarkers="center";styles.push_back(s);
        s=base;s.frameLayers=2;styles.push_back(s);
        s=base;s.stroke.pattern="none";s.fillAlpha=0.24F;styles.push_back(s);
        s=base;s.stroke.pattern="none";s.fillAlpha=0.16F;s.corners="solid";styles.push_back(s);
        s=base;s.diagonals="solid";styles.push_back(s);
        s=base;s.stroke.pattern="dashed";s.diagonals="dashed";styles.push_back(s);
        s=base;s.roundingPx=9;styles.push_back(s);
        s=base;s.roundingPx=9;s.stroke.pattern="dashed";styles.push_back(s);
        s=base;s.roundingPx=9;s.stroke.pattern="dash_dot";s.corners="solid";styles.push_back(s);
        int cases=0;
        for(float size:{16,32,64,128,256}) for(float aspect:{1.0F,1.6F})
        for(std::size_t i=0;i<styles.size();++i) {
            const auto& style=styles[i];
            const Point a{21.5F,33.5F},b{a.x+size*aspect,a.y+size};
            const auto lines=build(style,a,b);
            const auto seg=segments(lines);
            if(i==10) require(seg.empty(),"fill-only emitted strokes");
            else require(!seg.empty(),"missing strokes");
            if(i==0) require(lines.size()==1&&lines[0].closed&&lines[0].points.size()==4,
                "solid rectangle is not a four-vertex closed path");
            if(i==3) {
                require(lines.size()==4,"corner count");
                for(const auto& line:lines)
                    require(!line.closed&&line.points.size()==3,"invalid L corner");
            }
            for(const auto& e:seg) {
                require(distance(e.a,e.b)>0.001F,"degenerate segment");
                const float pad=style.edgeMarkers=="outside"?style.layerGapPx+style.markerPx:0.03F;
                for(auto p:{e.a,e.b})
                    require(std::isfinite(p.x)&&std::isfinite(p.y)&&p.x>=a.x-pad&&
                        p.x<=b.x+pad&&p.y>=a.y-pad&&p.y<=b.y+pad,"stroke outside bounds");
                for(bool horizontal:{true,false}) {
                    const auto reflect=[&](Point p) {
                        return horizontal?Point{a.x+b.x-p.x,p.y}:Point{p.x,a.y+b.y-p.y};
                    };
                    const auto u=reflect(e.a),v=reflect(e.b);
                    const bool found=std::any_of(seg.begin(),seg.end(),[&](const Segment& other){
                        return std::abs(e.width-other.width)<0.001F&&
                            ((near(u,other.a)&&near(v,other.b))||(near(u,other.b)&&near(v,other.a)));
                    });
                    if(!found) throw std::runtime_error("asymmetric style "+std::to_string(i)+
                        " size "+std::to_string(size));
                }
            }
            ++cases;
        }
        std::cout<<"VTR geometry: "<<cases<<" style/size/aspect combinations PASS\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
