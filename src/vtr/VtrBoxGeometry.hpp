#pragma once
#include "vtr/VtrCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>
#include <utility>

namespace reg::vtr::boxgeom {
struct Point { float x{}, y{}; };
using Path=std::vector<Point>;
enum class Part { Edge, Corner, Marker, Diagonal };
struct Line { Path points; float width{}; bool closed{}; Part part{Part::Edge}; };
using Lines=std::vector<Line>;

inline float distance(Point a,Point b) {
    return std::hypot(b.x-a.x,b.y-a.y);
}
inline void append(Path& p,Point q) {
    if(p.empty()||distance(p.back(),q)>0.001F) p.push_back(q);
}
inline Point interpolate(Point a,Point b,float t) {
    return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};
}
// Extract an arc-length interval, preserving genuine corner vertices.
inline Path interval(const Path& p,float start,float end) {
    Path result;
    float offset=0;
    for(std::size_t i=1;i<p.size();++i) {
        const float len=distance(p[i-1],p[i]);
        if(len<=0.001F) continue;
        const float lo=std::max(start,offset),hi=std::min(end,offset+len);
        if(hi-lo>0.001F) {
            append(result,interpolate(p[i-1],p[i],(lo-offset)/len));
            append(result,interpolate(p[i-1],p[i],(hi-offset)/len));
        }
        offset+=len;
    }
    return result;
}
// Center a symmetric repeating pattern on each path. Complete dashes only,
// with a small gap at each end; short paths get one centered short dash.
// The dash-dot cycle is symmetric about its long dash.
inline void pattern(Lines& out,const Path& raw,float width,std::string_view style,
                    bool closed=false,float dashPx=0,float gapPx=0,float dotPx=0) {
    if(style=="none") return;
    Path p;
    for(auto q:raw) append(p,q);
    if(closed&&p.size()>1&&distance(p.front(),p.back())<=0.001F) p.pop_back();
    if(p.size()<2) return;
    if(style=="solid") {
        const bool loop=closed&&p.size()>2;
        out.push_back({std::move(p),width,loop});
        return;
    }
    if(closed) append(p,p.front());
    float len=0;
    for(std::size_t i=1;i<p.size();++i) len+=distance(p[i-1],p[i]);
    if(!std::isfinite(len)||len<=0.001F) return;
    const bool dot=style=="dash_dot";
    float dash=dashPx>0?std::max(1.0F,dashPx):(dot?10.0F:7.0F);
    float gap=gapPx>0?std::max(1.0F,gapPx):(dot?4.0F:5.0F);
    float dotLen=dotPx>0?std::max(1.0F,dotPx):2.0F;
    // Bounded VTR tessellation: stretch the cycle rather than drop
    // remote dashes or allow tiny intervals to explode into millions.
    const float initialPeriod=dot?dash+2*gap+dotLen:dash+gap;
    const float scale=std::max(1.0F,len/(512*initialPeriod));
    dash*=scale;gap*=scale;dotLen*=scale;
    const float period=dot?dash+2*gap+dotLen:dash+gap;
    const float edgeGap=std::min(2.0F,len*0.15F);
    const float mid=len*0.5F;
    // Prefix arclength gives O(log N + covered segments) interval lookup
    // rather than scanning the entire path for every dash.
    std::vector<float> prefix(p.size(),0);
    for(std::size_t i=1;i<p.size();++i)prefix[i]=prefix[i-1]+distance(p[i-1],p[i]);
    auto emit=[&](float center,float length) {
        const float a=center-length*0.5F,b=center+length*0.5F;
        if(a<edgeGap-0.001F||b>len-edgeGap+0.001F) return;
        Path q;
        auto i=std::size_t(std::upper_bound(prefix.begin(),prefix.end(),a)-prefix.begin());
        i=std::max<std::size_t>(1,i);
        for(;i<p.size()&&prefix[i-1]<b;++i) {
            const float segment=prefix[i]-prefix[i-1];
            if(segment<=0.001F)continue;
            const float lo=std::max(a,prefix[i-1]),hi=std::min(b,prefix[i]);
            if(hi-lo>0.001F){
                append(q,interpolate(p[i-1],p[i],(lo-prefix[i-1])/segment));
                append(q,interpolate(p[i-1],p[i],(hi-prefix[i-1])/segment));
            }
        }
        if(q.size()>=2) out.push_back({std::move(q),width,false});
    };
    emit(mid,std::min(dash,len-2*edgeGap));
    for(int n=1;static_cast<float>(n)*period<mid+period;++n) {
        const float shift=static_cast<float>(n)*period;
        emit(mid-shift,dash); emit(mid+shift,dash);
        if(dot) { emit(mid-shift+period*0.5F,dotLen); emit(mid+shift-period*0.5F,dotLen); }
    }
}
inline std::array<Path,4> corners(Point a,Point b,float k,float r) {
    const Point centers[4]={{a.x+r,a.y+r},{b.x-r,a.y+r},
                            {b.x-r,b.y-r},{a.x+r,b.y-r}};
    const Point before[4]={{a.x,a.y+k},{b.x-k,a.y},{b.x,b.y-k},{a.x+k,b.y}};
    const Point after[4]={{a.x+k,a.y},{b.x,a.y+k},{b.x-k,b.y},{a.x,b.y-k}};
    constexpr float pi=3.14159265358979323846F;
    std::array<Path,4> result;
    for(int n=0;n<4;++n) {
        auto& p=result[static_cast<std::size_t>(n)];
        append(p,before[n]);
        if(r>0.001F) {
            const int steps=std::clamp(static_cast<int>(std::ceil(r*0.7F)),4,32);
            for(int i=0;i<=steps;++i) {
                const float angle=pi+(static_cast<float>(n)+
                    static_cast<float>(i)/static_cast<float>(steps))*pi*0.5F;
                append(p,{centers[n].x+r*std::cos(angle),centers[n].y+r*std::sin(angle)});
            }
        } else append(p,centers[n]);
        append(p,after[n]);
    }
    return result;
}
inline Box adapted(Box s,float span) {
    if(!s.adaptive) return s;
    const float maxWidth=std::max(1.0F,span*0.12F);
    s.stroke.width=std::min(s.stroke.width,maxWidth);
    s.cornerWidth=std::min(s.cornerWidth,maxWidth);
    s.markerWidth=std::min(s.markerWidth,maxWidth);
    s.diagonalWidth=std::min(s.diagonalWidth,maxWidth);
    s.outlineWidth=std::min(s.outlineWidth,std::max(0.0F,span*0.04F));
    s.cornerPx=std::min(s.cornerPx,span*0.30F);
    s.markerPx=std::min(s.markerPx,span*0.25F);
    s.roundingPx=std::min(s.roundingPx,span*0.20F);
    return s;
}
struct LabelBounds { float left{},top{},bottom{}; };
// Match the renderer's reference centerline snap and the integer raster
// footprint of outer edge/corner strokes. External markers are intentionally
// excluded: a label belongs to the main box, not to detached decorations.
inline Point snapPoint(const Box& s,Point p) {
    const float ref=s.stroke.pattern=="none"?s.cornerWidth:s.stroke.width;
    const float offset=(static_cast<int>(std::round(ref))%2)?0.5F:0.0F;
    return {std::round(p.x-offset)+offset,std::round(p.y-offset)+offset};
}
inline LabelBounds labelBounds(const Box& style,Point a,Point b) {
    const Box s=adapted(style,std::max(0.0F,std::min(b.x-a.x,b.y-a.y)));
    a=snapPoint(s,a);b=snapPoint(s,b);
    float width=0;
    if(s.stroke.pattern!="none"&&s.edgeAlpha>0) width=s.stroke.width;
    if(s.corners!="none"&&s.cornerAlpha>0) width=std::max(width,s.cornerWidth);
    if(width<=0) return {a.x,a.y,b.y};
    if(s.outlineAlpha>0) width+=2*s.outlineWidth;
    const float thick=std::max(1.0F,std::round(width));
    const auto low=[&](float x){return std::floor(x-thick*0.5F+0.5F);};
    if(s.roundingPx>0) {
        // Curves use geometric AA; take the conservative union with the
        // orthogonal footprint where a rounded frame has straight sides.
        return {std::min(low(a.x),a.x-width*0.5F),
            std::min(low(a.y),a.y-width*0.5F),
            std::max(low(b.y)+thick,b.y+width*0.5F)};
    }
    return {low(a.x),low(a.y),low(b.y)+thick};
}
inline Lines build(const Box& style,Point a,Point b) {
    Lines lines;
    const float w=b.x-a.x,h=b.y-a.y,span=std::min(w,h);
    if(!std::isfinite(span)||span<=0.0F) return lines;
    const Box s=adapted(style,span);
    auto add=[&](const Path& p,float width,std::string_view pat,Part part,bool closed=false) {
        const auto before=lines.size();
        pattern(lines,p,width,pat,closed,s.dashPx,s.gapPx,s.dotPx);
        for(auto i=before;i<lines.size();++i) lines[i].part=part;
    };
    const float radius=std::min(s.roundingPx,span*0.45F);
    auto layer=[&](float inset,bool edges,bool corner) {
        const Point lo{a.x+inset,a.y+inset},hi{b.x-inset,b.y-inset};
        const float side=std::min(hi.x-lo.x,hi.y-lo.y);
        if(side<=2.0F) return;
        const float r=std::clamp(radius-inset,0.0F,side*0.45F);
        const bool hasCorners=corner&&s.corners!="none";
        const float k=hasCorners?std::min(std::max(s.cornerPx,r),side*0.45F):r;
        const auto p=corners(lo,hi,k,r);
        if(edges&&s.stroke.pattern!="none") {
            if(!hasCorners&&s.stroke.pattern=="solid") {
                Path full;
                for(const auto& q:p) for(auto v:q) append(full,v);
                add(full,s.stroke.width,"solid",Part::Edge,true);
            } else {
                // Opposite sides have identical lengths and phase. Separate
                // rounded arcs are centered too; square corners have no arc.
                for(std::size_t i=0;i<4;++i)
                    add({p[i].back(),p[(i+1)%4].front()},
                        s.stroke.width,s.stroke.pattern,Part::Edge);
                if(!hasCorners&&r>0.001F)
                    for(const auto& q:p) add(q,s.stroke.width,s.stroke.pattern,Part::Edge);
            }
        }
        if(hasCorners)
            for(const auto& q:p) add(q,s.cornerWidth,s.corners,Part::Corner);
    };
    layer(0,true,true);
    if(s.cornerLayers==2||s.frameLayers==2) {
        const float gap=std::min(s.layerGapPx,span*0.24F);
        layer(gap,s.frameLayers==2,s.cornerLayers==2);
    }
    const float mx=(a.x+b.x)*0.5F,my=(a.y+b.y)*0.5F;
    const float k=std::min(s.markerPx*0.5F,span*0.22F);
    auto marker=[&](Point p,Point q) { add({p,q},s.markerWidth,"solid",Part::Marker); };
    if(s.edgeMarkers=="center") {
        marker({mx-k,a.y},{mx+k,a.y}); marker({mx-k,b.y},{mx+k,b.y});
        marker({a.x,my-k},{a.x,my+k}); marker({b.x,my-k},{b.x,my+k});
    } else if(s.edgeMarkers=="outside") {
        const float gap=s.layerGapPx;
        for(float x:{a.x,b.x}) for(float y:{a.y,b.y}) {
            const float sx=x==a.x?-1.0F:1.0F,sy=y==a.y?-1.0F:1.0F;
            marker({x+sx*gap,y},{x+sx*(gap+k),y});
            marker({x,y+sy*gap},{x,y+sy*(gap+k)});
        }
    }
    add({a,b},s.diagonalWidth,s.diagonals,Part::Diagonal);
    add({{b.x,a.y},{a.x,b.y}},s.diagonalWidth,s.diagonals,Part::Diagonal);
    return lines;
}
// Exact framebuffer-aligned orthogonal strokes. At 1x DPI the requested
// width is rounded to a whole pixel (minimum one). Open ends are butt caps;
// interior vertices have square miter joins. Non-overlapping rectangles are
// returned so a translucent underlay cannot accumulate alpha at the joins.
struct PixelRect { int x0{},y0{},x1{},y1{}; };
inline bool orthogonal(const Line& line) {
    if(line.points.size()<2) return false;
    const std::size_t n=line.points.size();
    for(std::size_t i=0;i<(line.closed?n:n-1);++i) {
        const auto a=line.points[i],b=line.points[(i+1)%n];
        if(std::abs(a.x-b.x)>0.001F&&std::abs(a.y-b.y)>0.001F) return false;
    }
    return true;
}
inline std::vector<PixelRect> rawPixelRects(const Line& line,float width) {
    if(!orthogonal(line)) return {};
    const int thick=std::max(1,static_cast<int>(std::round(width)));
    const auto round=[](float x) { return static_cast<int>(std::floor(x+0.5F)); };
    const auto lo=[&](float x) { return round(x-thick*0.5F); };
    std::vector<PixelRect> source;
    auto add=[&](PixelRect r) { if(r.x1>r.x0&&r.y1>r.y0) source.push_back(r); };
    const auto n=line.points.size();
    for(std::size_t i=0;i<(line.closed?n:n-1);++i) {
        const Point a=line.points[i],b=line.points[(i+1)%n];
        if(std::abs(a.y-b.y)<=0.001F)
            add({round(std::min(a.x,b.x)),lo(a.y),
                 round(std::max(a.x,b.x)),lo(a.y)+thick});
        else
            add({lo(a.x),round(std::min(a.y,b.y)),
                 lo(a.x)+thick,round(std::max(a.y,b.y))});
    }
    // Square joins cover the entire outside quadrant without relying on
    // a renderer-specific textured-line normal or triangle interpolation.
    for(std::size_t i=line.closed?0:1;i<(line.closed?n:n-1);++i) {
        const auto p=line.points[i];
        add({lo(p.x),lo(p.y),lo(p.x)+thick,lo(p.y)+thick});
    }
    return source;
}
inline std::vector<PixelRect> unionRects(const std::vector<PixelRect>& source) {
    // Decompose the rectangle union into disjoint x-slabs, coalescing y
    // intervals. This is bounded by the small number of path vertices.
    std::vector<int> xs;
    for(const auto& r:source) { xs.push_back(r.x0); xs.push_back(r.x1); }
    std::sort(xs.begin(),xs.end());
    xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
    std::vector<PixelRect> result;
    for(std::size_t i=1;i<xs.size();++i) {
        const int x0=xs[i-1],x1=xs[i];
        std::vector<std::pair<int,int>> spans;
        for(const auto& r:source)
            if(r.x0<=x0&&r.x1>=x1) spans.emplace_back(r.y0,r.y1);
        std::sort(spans.begin(),spans.end());
        for(std::size_t j=0;j<spans.size();) {
            const int y0=spans[j].first;
            int y1=spans[j++].second;
            while(j<spans.size()&&spans[j].first<=y1)
                y1=std::max(y1,spans[j++].second);
            result.push_back({x0,y0,x1,y1});
        }
    }
    return result;
}
inline std::vector<PixelRect> pixelRects(const Line& line,float width) {
    return unionRects(rawPixelRects(line,width));
}

// For translucent mixed (axis + curved/diagonal) underlays, compose the
// entire stroke union before blending. Scan-convert convex stroke pieces
// at 4x subpixel coverage; never blend each individual overlapping stroke.
struct CoverageRun { int x0{},x1{},y{}; unsigned samples{}; };
template<bool Reference = false, class Emit>
void mixedCoverage(const Lines& lines,float extra,PixelRect clip,Emit emit) {
    constexpr int S=4;
    const int width=clip.x1-clip.x0,height=clip.y1-clip.y0;
    if(width<=0||height<=0) return;
    using Span=std::pair<int,int>;
    std::vector<std::vector<Span>> rows(static_cast<std::size_t>(height)*S);
    auto polygon=[&](const Path& p) {
        if(p.size()<3) return;
        float ymin=p.front().y,ymax=ymin;
        for(auto q:p) { ymin=std::min(ymin,q.y);ymax=std::max(ymax,q.y); }
        const int first=std::max(0,static_cast<int>(std::ceil((ymin-clip.y0)*S-0.5F)));
        const int last=std::min(height*S,static_cast<int>(std::ceil((ymax-clip.y0)*S-0.5F)));
        for(int y=first;y<last;++y) {
            const float sy=clip.y0+(y+0.5F)/S;
            float lo=1.0e30F,hi=-1.0e30F;
            for(std::size_t i=0;i<p.size();++i) {
                const auto a=p[i],b=p[(i+1)%p.size()];
                if((a.y<=sy&&sy<b.y)||(b.y<=sy&&sy<a.y)) {
                    const float x=a.x+(b.x-a.x)*(sy-a.y)/(b.y-a.y);
                    lo=std::min(lo,x);hi=std::max(hi,x);
                }
            }
            if(lo>=hi) continue;
            const int x0=std::max(0,static_cast<int>(std::ceil((lo-clip.x0)*S-0.5F)));
            const int x1=std::min(width*S,static_cast<int>(std::ceil((hi-clip.x0)*S-0.5F)));
            if(x1>x0) rows[static_cast<std::size_t>(y)].emplace_back(x0,x1);
        }
    };
    for(const auto& line:lines) {
        const float thick=line.width+extra;
        if(orthogonal(line)) {
            for(auto r:rawPixelRects(line,thick))
                polygon({{float(r.x0),float(r.y0)},{float(r.x1),float(r.y0)},
                         {float(r.x1),float(r.y1)},{float(r.x0),float(r.y1)}});
            continue;
        }
        const auto& p=line.points;
        const std::size_t n=p.size(),count=line.closed?n:n-1;
        const float half=thick*0.5F;
        std::vector<Point> normals;
        normals.reserve(count);
        for(std::size_t i=0;i<count;++i) {
            auto a=p[i],b=p[(i+1)%n];
            const float len=distance(a,b);
            normals.push_back(len>0.001F?Point{-(b.y-a.y)/len,(b.x-a.x)/len}:Point{});
        }
        // Segment strips and bounded miter wedges. All pieces are convex.
        for(std::size_t i=0;i<count;++i) {
            const auto a=p[i],b=p[(i+1)%n],q=normals[i];
            polygon({{a.x+q.x*half,a.y+q.y*half},{b.x+q.x*half,b.y+q.y*half},
                     {b.x-q.x*half,b.y-q.y*half},{a.x-q.x*half,a.y-q.y*half}});
        }
        for(std::size_t i=line.closed?0:1;i<(line.closed?n:n-1);++i) {
            const auto a=p[i],u=normals[(i+count-1)%count],v=normals[i%count];
            const float den=1+u.x*v.x+u.y*v.y;
            for(float sign:{-1.0F,1.0F}) {
                const Point q{a.x+sign*u.x*half,a.y+sign*u.y*half};
                const Point r{a.x+sign*v.x*half,a.y+sign*v.y*half};
                // Bevel extremely acute joins instead of emitting a spike.
                if(den>0.125F) {
                    const Point m{a.x+sign*(u.x+v.x)*half/den,
                                  a.y+sign*(u.y+v.y)*half/den};
                    polygon({a,q,m,r});
                } else polygon({a,q,r});
            }
        }
    }
    if constexpr (Reference) {
        std::vector<unsigned> coverage(static_cast<std::size_t>(width));
        for(int y=0;y<height;++y) {
            std::fill(coverage.begin(),coverage.end(),0U);
            for(int sy=0;sy<S;++sy) {
                auto& spans=rows[static_cast<std::size_t>(y*S+sy)];
                std::sort(spans.begin(),spans.end());
                for(std::size_t j=0;j<spans.size();) {
                    const int x0=spans[j].first;
                    int x1=spans[j++].second;
                    while(j<spans.size()&&spans[j].first<=x1)
                        x1=std::max(x1,spans[j++].second);
                    for(int x=x0;x<x1;++x) ++coverage[static_cast<std::size_t>(x/S)];
                }
            }
            for(int x=0;x<width;) {
                const int begin=x;
                const unsigned value=coverage[static_cast<std::size_t>(x++)];
                while(x<width&&coverage[static_cast<std::size_t>(x)]==value) ++x;
                if(value) emit(CoverageRun{begin+clip.x0,x+clip.x0,y+clip.y0,value});
            }
        }
    } else {
        // The spans are disjoint per subpixel row after merging. Add fractional
        // endpoints explicitly and accumulate complete pixels via a difference array.
        std::vector<int> partial(width), delta(width + 1);
        for (int y = 0; y < height; ++y) {
            int first = width, last = -1;
            for (int sy = 0; sy < S; ++sy) {
                auto& spans = rows[static_cast<std::size_t>(y*S+sy)];
                std::sort(spans.begin(), spans.end());
                for (std::size_t j = 0; j < spans.size();) {
                    const int x0 = spans[j].first;
                    int x1 = spans[j++].second;
                    while (j < spans.size() && spans[j].first <= x1)
                        x1 = std::max(x1, spans[j++].second);
                    const int a = x0/S, b = (x1-1)/S;
                    first = std::min(first, a);
                    last = std::max(last, b);
                    if (a == b) {
                        partial[a] += x1-x0;
                    } else {
                        partial[a] += (a+1)*S-x0;
                        partial[b] += x1-b*S;
                        delta[a+1] += S;
                        delta[b] -= S;
                    }
                }
            }
            int carry = 0, value = 0, begin = first;
            for (int x = first; x <= last; ++x) {
                carry += delta[x];
                const int next = partial[x] + carry;
                partial[x] = delta[x] = 0;
                if (next == value)
                    continue;
                if (value)
                    emit(CoverageRun{begin+clip.x0,x+clip.x0,y+clip.y0,unsigned(value)});
                begin = x;
                value = next;
            }
            if (value)
                emit(CoverageRun{begin+clip.x0,last+1+clip.x0,y+clip.y0,unsigned(value)});
        }
    }
}
} // namespace reg::vtr::boxgeom
