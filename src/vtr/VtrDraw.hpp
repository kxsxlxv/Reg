#pragma once
#include "vtr/VtrBoxGeometry.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace reg::vtr::draw {
inline ImU32 rgba(Color c, float alpha = 1.0F) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r,c.g,c.b,alpha));
}
using Path=std::vector<ImVec2>;
inline void paintLine(ImDrawList* d,const boxgeom::Line& line,ImU32 color,float width) {
    if(boxgeom::orthogonal(line)) {
        // Filled, integer-aligned rectangles are exact for horizontal and
        // vertical strokes, including L joins. No baked line texture.
        for(const auto& r:boxgeom::pixelRects(line,width))
            d->AddRectFilled({static_cast<float>(r.x0),static_cast<float>(r.y0)},
                {static_cast<float>(r.x1),static_cast<float>(r.y1)},color);
        return;
    }
    Path points;
    points.reserve(line.points.size());
    for(auto p:line.points) points.emplace_back(p.x,p.y);
    if(points.size()<2) return;
    // Closed paths have unique vertices, and open L corners have exactly
    // three non-coincident points: ImGui can generate a proper miter join.
    // Keep geometric antialiasing for diagonals/curves. The software SDL
    // backend can distort baked line textures; do not use them for OSD.
    const ImDrawListFlags saved=d->Flags;
    d->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    d->AddPolyline(points.data(),static_cast<int>(points.size()),color,width,
        line.closed?ImDrawFlags_Closed:ImDrawFlags_None);
    d->Flags=saved;
}
inline void stroke(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 color,
            float width,std::string_view pattern) {
    boxgeom::Lines lines;
    boxgeom::pattern(lines,{{a.x,a.y},{b.x,b.y}},width,pattern);
    for(const auto& line:lines) paintLine(d,line,color,line.width);
}
inline void rectangle(ImDrawList* d,const Box& style,ImVec2 a,ImVec2 b) {
    const Box s=boxgeom::adapted(style,std::max(0.0F,std::min(b.x-a.x,b.y-a.y)));
    // Snap the reference centerlines once, not individual vertices.
    // Odd integer widths use half-pixel centers; even use integer centers.
    const auto sa=boxgeom::snapPoint(s,{a.x,a.y}),sb=boxgeom::snapPoint(s,{b.x,b.y});
    a={sa.x,sa.y}; b={sb.x,sb.y};
    const float span=std::min(b.x-a.x,b.y-a.y);
    if(span<=0) return;
    const float radius=std::min(s.roundingPx,span*0.45F);
    if(s.fillAlpha>0) d->AddRectFilled(a,b,rgba(s.fill,s.fillAlpha),radius);
    Box effective=s;effective.adaptive=false;
    auto lines=boxgeom::build(effective,{a.x,a.y},{b.x,b.y});
    const auto alphaFor=[&](boxgeom::Part part) {
        return part==boxgeom::Part::Corner?s.cornerAlpha:
            part==boxgeom::Part::Marker?s.markerAlpha:s.edgeAlpha;
    };
    lines.erase(std::remove_if(lines.begin(),lines.end(),[&](const auto& line){
        return alphaFor(line.part)<=0;
    }),lines.end());
    // One complete underlay pass, then one complete color pass. Underlays
    // must never erase an adjacent colored edge/corner/marker.
    if(s.outlineWidth>0&&s.outlineAlpha>0) {
        const float extra=2*s.outlineWidth;
        const auto ink=rgba(s.outlineColor,s.outlineAlpha);
        if(s.outlineAlpha>=1.0F) {
            // Preserve the established opaque renderer and its fast path.
            for(const auto& line:lines) paintLine(d,line,ink,line.width+extra);
        } else if(std::all_of(lines.begin(),lines.end(),boxgeom::orthogonal)) {
            std::vector<boxgeom::PixelRect> pieces;
            for(const auto& line:lines) {
                auto rects=boxgeom::rawPixelRects(line,line.width+extra);
                pieces.insert(pieces.end(),rects.begin(),rects.end());
            }
            // Union across ALL components, not only each path.
            for(const auto& r:boxgeom::unionRects(pieces))
                d->AddRectFilled({float(r.x0),float(r.y0)},{float(r.x1),float(r.y1)},ink);
        } else {
            // Bound CPU coverage to the visible bbox + maximum stroke extent.
            const float pad=2*(std::max({s.stroke.width,s.cornerWidth,
                s.markerWidth,s.diagonalWidth})+extra)+s.layerGapPx+s.markerPx+3;
            const auto lo=d->GetClipRectMin(),hi=d->GetClipRectMax();
            const boxgeom::PixelRect clip{
                int(std::floor(std::max(lo.x,a.x-pad))),
                int(std::floor(std::max(lo.y,a.y-pad))),
                int(std::ceil(std::min(hi.x,b.x+pad))),
                int(std::ceil(std::min(hi.y,b.y+pad)))};
            boxgeom::mixedCoverage(lines,extra,clip,[&](boxgeom::CoverageRun run) {
                d->AddRectFilled({float(run.x0),float(run.y)},
                    {float(run.x1),float(run.y+1)},
                    rgba(s.outlineColor,s.outlineAlpha*float(run.samples)/16.0F));
            });
        }
    }
    // Component colors/alpha are separate semantic layers. Order is
    // edges/diagonals, then corners, then markers.
    for(auto part:{boxgeom::Part::Edge,boxgeom::Part::Diagonal,boxgeom::Part::Corner,boxgeom::Part::Marker}) {
        const Color color=part==boxgeom::Part::Corner?s.cornerColor.value_or(s.stroke.color):
            part==boxgeom::Part::Marker?s.markerColor.value_or(s.stroke.color):s.stroke.color;
        for(const auto& line:lines)
            if(line.part==part) paintLine(d,line,rgba(color,alphaFor(part)),line.width);
    }
}
inline void drawLabel(ImDrawList* d,const Output& o,ImVec2 a,ImVec2 b,
               ImVec2 min,ImVec2 max,std::string_view resolved) {
    if(!o.label.show||resolved.empty()||max.x-min.x<=2.0F||max.y-min.y<=2.0F) return;
    const auto& s=o.label;
    std::string text(resolved);
    auto* font=ImGui::GetFont();
    float px=s.fontPx;
    const float pad=s.paddingPx;
    const float availableW=std::max(1.0F,std::min(max.x-min.x-2.0F,
        s.maxWidthPx>0?s.maxWidthPx:max.x-min.x-2.0F));
    const float availableH=std::max(1.0F,max.y-min.y-2.0F);
    const auto measure=[&](){return font->CalcTextSizeA(px,1.0e6F,0.0F,text.c_str());};
    ImVec2 size=measure();
    while(px>8.0F && (size.x+2*pad>availableW || size.y+2*pad>availableH)) {
        px=std::max(8.0F,px-1.0F); size=measure();
    }
    if(size.x+2*pad>availableW) {
        // Trim at UTF-8 boundaries and append ASCII ellipsis.
        while(!text.empty()) {
            std::size_t cut=text.size()-1;
            while(cut>0&&(static_cast<unsigned char>(text[cut])&0xC0)==0x80) --cut;
            text.resize(cut);
            size=font->CalcTextSizeA(px,1.0e6F,0.0F,(text+"...").c_str());
            if(size.x+2*pad<=availableW) break;
        }
        text+="...";
        size=measure();
    }
    const float width=std::min(availableW,size.x+2*pad);
    const float height=std::min(availableH,size.y+2*pad);
    const auto border=boxgeom::labelBounds(o.box,{a.x,a.y},{b.x,b.y});
    const float above=border.top-s.gapPx-height, below=border.bottom+s.gapPx;
    const bool topFits=above>=min.y+1.0F;
    const bool bottomFits=below+height<=max.y-1.0F;
    float y=above;
    if(s.place=="bottom_left_outside")
        y=bottomFits?below:(topFits?above:b.y-height);
    else
        y=topFits?above:(bottomFits?below:a.y);
    y=std::clamp(y,min.y+1.0F,max.y-1.0F-height);
    const float x=std::clamp(border.left,min.x+1.0F,max.x-1.0F-width);
    const Color bg=s.boxBackground?o.box.stroke.color:s.background;
    const Color fg=s.autoColor?contrastingText(bg):s.color;
    const ImVec2 end{x+width,y+height};
    d->AddRectFilled({x,y},end,rgba(bg,s.backgroundAlpha),s.roundingPx);
    d->PushClipRect({x,y},end,true);
    d->AddText(font,px,{x+pad,y+pad},rgba(fg),text.c_str());
    d->PopClipRect();
}
inline void outline(ImDrawList* d,const std::vector<ImVec2>& points,const Osd& o,bool closed) {
    if(points.size()<2||points.size()>4096||o.alpha<=0) return;
    boxgeom::Path path;
    for(auto p:points) boxgeom::append(path,{p.x,p.y});
    boxgeom::Lines lines;
    boxgeom::pattern(lines,path,o.stroke.width,o.stroke.pattern,closed,o.dashPx,o.gapPx,o.dotPx);
    if(o.outlineWidth>0&&o.outlineAlpha>0) {
        // Same clipped union coverage as BBOX for a translucent underlay.
        const float extra=2*o.outlineWidth;
        if(o.outlineAlpha>=1) {
            for(const auto& line:lines) paintLine(d,line,rgba(o.outlineColor),line.width+extra);
        } else if(!lines.empty()) {
            float x0=points[0].x,x1=x0,y0=points[0].y,y1=y0;
            for(auto p:points) {x0=std::min(x0,p.x);x1=std::max(x1,p.x);y0=std::min(y0,p.y);y1=std::max(y1,p.y);}
            const auto lo=d->GetClipRectMin(),hi=d->GetClipRectMax();
            const float pad=4*(o.stroke.width+extra)+2;
            boxgeom::PixelRect clip{int(std::floor(std::max(lo.x,x0-pad))),int(std::floor(std::max(lo.y,y0-pad))),
                int(std::ceil(std::min(hi.x,x1+pad))),int(std::ceil(std::min(hi.y,y1+pad)))};
            boxgeom::mixedCoverage(lines,extra,clip,[&](boxgeom::CoverageRun run){
                d->AddRectFilled({float(run.x0),float(run.y)},{float(run.x1),float(run.y+1)},
                    rgba(o.outlineColor,o.outlineAlpha*float(run.samples)/16));
            });
        }
    }
    for(const auto& line:lines) paintLine(d,line,rgba(o.stroke.color,o.alpha),line.width);
}
inline std::vector<ImVec2> ellipsePath(ImVec2 center,float rx,float ry) {
    std::vector<ImVec2> p;
    // Cap tessellation; approximate curve in display coordinates.
    const int count=std::clamp(int(std::ceil(std::max(rx,ry)*0.8F)),16,96);
    for(int i=0;i<count;++i) {
        const float angle=6.28318530718F*i/count;
        p.emplace_back(center.x+rx*std::cos(angle),center.y+ry*std::sin(angle));
    }
    return p;
}
inline void filledPath(ImDrawList* d,const std::vector<ImVec2>& p,const Osd& o) {
    if(p.size()<3||o.fillAlpha<=0) return;
    d->AddConvexPolyFilled(p.data(),static_cast<int>(p.size()),
        rgba(o.fill.value_or(o.stroke.color),o.fillAlpha));
}
} // namespace reg::vtr::draw
