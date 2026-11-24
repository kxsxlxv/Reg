#include "vtr/VtrLegend.hpp"
#include "vtr/VtrPhoto.hpp"
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace reg::vtr {
namespace {
ImU32 rgba(Color c, float alpha = 1.0F) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r,c.g,c.b,alpha));
}
// Continuous dash phase along a path, including rounded corners.
using Path = std::vector<ImVec2>;
void strokePath(ImDrawList* d, const Path& points, ImU32 color,
                float width, std::string_view pattern, float border=0.0F) {
    if (pattern=="none" || points.size()<2U) return;
    const auto paint=[&](const Path& p) {
        if (p.size()<2U) return;
        if (border>0.0F)
            d->AddPolyline(p.data(),static_cast<int>(p.size()),
                IM_COL32(12,16,20,235),width+2.0F*border,ImDrawFlags_None);
        d->AddPolyline(p.data(),static_cast<int>(p.size()),color,width,ImDrawFlags_None);
    };
    if (pattern=="solid") { paint(points); return; }
    const bool dot=pattern=="dash_dot";
    const std::array<float,4> lengths=dot
        ? std::array<float,4>{10,4,2,4}
        : std::array<float,4>{7,5,7,5};
    int phase=0;
    float remaining=lengths[0];
    Path run;
    for (std::size_t i=1;i<points.size();++i) {
        const ImVec2 a=points[i-1], b=points[i];
        const float dx=b.x-a.x,dy=b.y-a.y,len=std::hypot(dx,dy);
        if (len<0.001F) continue;
        for (float s=0;s<len-0.0001F;) {
            const float step=std::min(remaining,len-s);
            if ((phase%2)==0) {
                if (run.empty()) run.emplace_back(a.x+dx*s/len,a.y+dy*s/len);
                run.emplace_back(a.x+dx*(s+step)/len,a.y+dy*(s+step)/len);
            }
            s+=step; remaining-=step;
            if (remaining<0.0001F) {
                if ((phase%2)==0) { paint(run); run.clear(); }
                phase=(phase+1)%4; remaining=lengths[static_cast<std::size_t>(phase)];
            }
        }
    }
    paint(run);
}
void stroke(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 color,
            float width, std::string_view pattern) {
    strokePath(d,{a,b},color,width,pattern);
}
std::string replace(std::string text,std::string_view key,
                    std::string_view value) {
    for (size_t pos=0;(pos=text.find(key,pos))!=std::string::npos;
         pos+=value.size())
        text.replace(pos,key.size(),value);
    return text;
}
std::string sampleLabel(std::string text) {
    text=replace(std::move(text),"{track_id}","42");
    text=replace(std::move(text),"{id}","42");
    text=replace(std::move(text),"{conf_pct}","87");
    text=replace(std::move(text),"{class}","UAV");
    text=replace(std::move(text),"{class_id}","0");
    text=replace(std::move(text),"{producer}","NN");
    text=replace(std::move(text),"{origin}","Canny");
    return text;
}
// Construct four corner paths clockwise. Their straight legs may have
// different lengths from the rounding radius. The underlying BBOX is unchanged.
std::array<Path,4> cornerPaths(ImVec2 a,ImVec2 b,float k,float r) {
    const ImVec2 c[4]={{a.x+r,a.y+r},{b.x-r,a.y+r},
                      {b.x-r,b.y-r},{a.x+r,b.y-r}};
    const ImVec2 before[4]={{a.x,a.y+k},{b.x-k,a.y},
                            {b.x,b.y-k},{a.x+k,b.y}};
    const ImVec2 after[4]={{a.x+k,a.y},{b.x,a.y+k},
                           {b.x-k,b.y},{a.x,b.y-k}};
    constexpr float pi=3.14159265358979323846F;
    std::array<Path,4> paths;
    for(int n=0;n<4;++n) {
        auto& p=paths[static_cast<std::size_t>(n)];
        p.push_back(before[n]);
        if(r>0.0F) {
            for(int i=0;i<=6;++i) {
                const float angle=pi+(static_cast<float>(n)+static_cast<float>(i)/6.0F)*pi*0.5F;
                p.emplace_back(c[n].x+r*std::cos(angle),c[n].y+r*std::sin(angle));
            }
        } else p.push_back(c[n]);
        p.push_back(after[n]);
    }
    return paths;
}
void rectangle(ImDrawList* d, const Box& s, ImVec2 a, ImVec2 b) {
    const ImU32 line=rgba(s.stroke.color);
    const float w=b.x-a.x,h=b.y-a.y;
    if(w<=0.0F||h<=0.0F) return;
    const float radius=std::min(s.roundingPx,std::min(w,h)*0.45F);
    if (s.fillAlpha>0.0F)
        d->AddRectFilled(a,b,rgba(s.fill,s.fillAlpha),radius);
    const auto drawLayer=[&](float inset,bool edges,bool corners) {
        const ImVec2 lo{a.x+inset,a.y+inset},hi{b.x-inset,b.y-inset};
        const float span=std::min(hi.x-lo.x,hi.y-lo.y);
        if(span<=2.0F) return;
        const float r=std::clamp(radius-inset,0.0F,span*0.45F);
        const float k=std::min(std::max(s.cornerPx,r),span*0.45F);
        const bool hasCorners=corners&&s.corners!="none";
        const auto paths=cornerPaths(lo,hi,hasCorners?k:r,r);
        if(edges && s.stroke.pattern!="none") {
            if(hasCorners) {
                for(std::size_t n=0;n<4;++n)
                    strokePath(d,{paths[n].back(),paths[(n+1)%4].front()},
                        line,s.stroke.width,s.stroke.pattern,s.outlineWidth);
            } else {
                Path perimeter;
                for(const auto& p:paths) perimeter.insert(perimeter.end(),p.begin(),p.end());
                perimeter.push_back(perimeter.front());
                strokePath(d,perimeter,line,s.stroke.width,s.stroke.pattern,s.outlineWidth);
            }
        }
        if(hasCorners)
            for(const auto& p:paths)
                strokePath(d,p,line,s.cornerWidth,s.corners,s.outlineWidth);
    };
    drawLayer(0.0F,true,true);
    if(s.cornerLayers==2||s.frameLayers==2)
        drawLayer(s.layerGapPx,s.frameLayers==2,s.cornerLayers==2);
    const float mx=(a.x+b.x)*0.5F,my=(a.y+b.y)*0.5F;
    const float k=std::min(s.markerPx*0.5F,std::min(w,h)*0.22F);
    const auto marker=[&](ImVec2 p,ImVec2 q) {
        strokePath(d,{p,q},line,s.markerWidth,"solid",s.outlineWidth);
    };
    if(s.edgeMarkers=="center") {
        marker({mx-k,a.y},{mx+k,a.y}); marker({mx-k,b.y},{mx+k,b.y});
        marker({a.x,my-k},{a.x,my+k}); marker({b.x,my-k},{b.x,my+k});
    } else if(s.edgeMarkers=="outside") {
        const float g=s.layerGapPx,len=k;
        for(float x:{a.x,b.x}) for(float y:{a.y,b.y}) {
            const float sx=x==a.x?-1.0F:1.0F,sy=y==a.y?-1.0F:1.0F;
            marker({x+sx*g,y},{x+sx*(g+len),y});
            marker({x,y+sy*g},{x,y+sy*(g+len)});
        }
    }
    strokePath(d,{a,b},line,s.diagonalWidth,s.diagonals,s.outlineWidth);
    strokePath(d,{{b.x,a.y},{a.x,b.y}},line,s.diagonalWidth,s.diagonals,s.outlineWidth);
}
void drawLabel(ImDrawList* d,const Output& o,ImVec2 a,ImVec2 b,
               ImVec2 min,ImVec2 max) {
    if(!o.label.show||o.label.text.empty()||max.x-min.x<=2.0F||max.y-min.y<=2.0F) return;
    const auto& s=o.label;
    const std::string text=sampleLabel(s.text);
    auto* font=ImGui::GetFont();
    float px=s.fontPx;
    const float pad=s.paddingPx;
    const float availableW=std::max(1.0F,max.x-min.x-2.0F);
    const float availableH=std::max(1.0F,max.y-min.y-2.0F);
    const auto measure=[&](){return font->CalcTextSizeA(px,1.0e6F,0.0F,text.c_str());};
    ImVec2 size=measure();
    while(px>8.0F && (size.x+2*pad>availableW || size.y+2*pad>availableH)) {
        px=std::max(8.0F,px-1.0F); size=measure();
    }
    const float width=std::min(availableW,size.x+2*pad);
    const float height=std::min(availableH,size.y+2*pad);
    const float above=a.y-s.gapPx-height, below=b.y+s.gapPx;
    const bool topFits=above>=min.y+1.0F;
    const bool bottomFits=below+height<=max.y-1.0F;
    float y=above;
    if(s.place=="bottom_left_outside")
        y=bottomFits?below:(topFits?above:b.y-height);
    else
        y=topFits?above:(bottomFits?below:a.y);
    y=std::clamp(y,min.y+1.0F,max.y-1.0F-height);
    const float x=std::clamp(a.x,min.x+1.0F,max.x-1.0F-width);
    const Color bg=s.boxBackground?o.box.stroke.color:s.background;
    const Color fg=s.autoColor?contrastingText(bg):s.color;
    const ImVec2 end{x+width,y+height};
    d->AddRectFilled({x,y},end,rgba(bg,s.backgroundAlpha),s.roundingPx);
    d->PushClipRect({x,y},end,true);
    d->AddText(font,px,{x+pad,y+pad},rgba(fg),text.c_str());
    d->PopClipRect();
}
void outline(ImDrawList* d, const std::vector<ImVec2>& points,
             const Osd& o, bool closed) {
    for (size_t i=1;i<points.size();++i)
        stroke(d,points[i-1],points[i],rgba(o.stroke.color,o.alpha),
               o.stroke.width,o.stroke.pattern);
    if(closed && points.size()>2U)
        stroke(d,points.back(),points.front(),rgba(o.stroke.color,o.alpha),
               o.stroke.width,o.stroke.pattern);
}
void preview(ImDrawList* d,const Output& o,ImVec2 min,ImVec2 max) {
    d->AddRectFilled(min,max,IM_COL32(12,23,35,255),5.0F);
    d->AddRect(min,max,IM_COL32(66,84,104,180),5.0F);
    d->PushClipRect(min,max,true);

    // This is the authentic user's 1536x804 photograph, sampled at 320x168
    // for a small atlas tile. Aspect-fit without cropping or stretching.
    PreviewPhoto photo{};
    const bool hasPhoto=previewPhoto(photo);
    const float width=max.x-min.x, height=max.y-min.y;
    const float srcW=hasPhoto?static_cast<float>(photo.width):320.0F;
    const float srcH=hasPhoto?static_cast<float>(photo.height):168.0F;
    // Reserve label room before fitting the photo, so labels don't occlude
    // the aircraft merely because a preview card has tight image margins.
    const float labelRoom=o.label.show&&!o.label.text.empty()
        ? std::min(height*0.30F,o.label.fontPx+2.0F*o.label.paddingPx+o.label.gapPx+4.0F) : 0.0F;
    const float scale=std::min(width/srcW,(height-labelRoom)/srcH);
    const ImVec2 photoMin{min.x+(width-srcW*scale)*0.5F,
                          min.y+labelRoom+(height-labelRoom-srcH*scale)*0.5F};
    const ImVec2 photoMax{photoMin.x+srcW*scale,photoMin.y+srcH*scale};
    if (hasPhoto) {
        d->AddImage(photo.texture,photoMin,photoMax,photo.uv0,photo.uv1,
                    IM_COL32(255,255,255,255));
    } else {
        d->AddText({min.x+8,min.y+8},IM_COL32(255,115,100,255),
                   "VTR photo unavailable");
    }

    // The reference BBOX is a prior manually measured annotation, not GT
    // produced by a neural net. All overlay geometry uses SOURCE image units
    // so boxes and contours remain aligned when the preview gets resized.
    // Source: 1536x804, [x1,y1,x2,y2] = [130,55,1111,671].
    const auto img=[&](float x,float y) -> ImVec2 {
        return {photoMin.x+x/1536.0F*(photoMax.x-photoMin.x),
                photoMin.y+y/804.0F*(photoMax.y-photoMin.y)};
    };
    const ImVec2 a=img(130,55),b=img(1111,671);
    const ImVec2 center=img(660,365);
    const ImU32 ink=rgba(o.osd.stroke.color,o.osd.alpha);
    const auto& geometry=o.geometry;
    if (geometry=="bbox") {
        rectangle(d,o.box,a,b);
        drawLabel(d,o,a,b,min,max);
    } else if (geometry=="line") {
        stroke(d,img(155,660),img(1105,65),ink,o.osd.stroke.width,
               o.osd.stroke.pattern);
    } else if (geometry=="arrow") {
        const ImVec2 p=img(630,290),q=img(915,460);
        stroke(d,p,q,ink,o.osd.stroke.width,o.osd.stroke.pattern);
        d->AddTriangleFilled(q,{q.x-10,q.y-13},{q.x-12,q.y+5},ink);
    } else if (geometry=="polyline") {
        // Handpicked, approximate aircraft boundary points; visualization
        // only, not a pixel-accurate segmentation mask or a GT source.
        outline(d,{img(140,650),img(153,665),img(674,404),
                   img(708,392),img(866,471),img(902,477),
                   img(920,457),img(892,417),img(768,323),
                   img(1105,65),img(1081,59),img(758,256),
                   img(541,123),img(512,103),img(443,199),
                   img(474,237),img(628,350),img(604,370),
                   img(140,650)},o.osd,false);
    } else if (geometry=="polygon" || geometry=="mask" ||
               geometry=="heatmap") {
        if (geometry=="mask" && o.semantic=="sky") {
            // Illustrative sky region outside the annotated object extent;
            // not a decoded per-pixel sky segmentation.
            const ImU32 c=rgba(o.osd.stroke.color,o.osd.alpha);
            d->AddRectFilled(photoMin,{photoMax.x,a.y},c);
            d->AddRectFilled({photoMin.x,b.y},photoMax,c);
            d->AddRectFilled({photoMin.x,a.y},{a.x,b.y},c);
            d->AddRectFilled({b.x,a.y},{photoMax.x,b.y},c);
        } else if (geometry=="heatmap") {
            for (int i=4;i>=1;--i)
                d->AddCircleFilled(img(810,437),10.0F*i,
                    rgba(o.osd.stroke.color,o.osd.alpha*0.13F));
        } else {
            const ImVec2 points[]={
                img(520,325),img(713,249),img(915,455),
                img(818,501),img(594,410)};
            const float alpha=geometry=="mask"?o.osd.alpha:0.22F;
            d->AddConvexPolyFilled(points,5,rgba(o.osd.stroke.color,alpha));
            if (geometry=="polygon")
                outline(d,{points[0],points[1],points[2],points[3],
                           points[4]},o.osd,true);
        }
    } else if (geometry=="circle") {
        d->AddCircle(center,30.0F,ink,0,o.osd.stroke.width);
    } else if (geometry=="ellipse") {
        std::vector<ImVec2> points;
        for (int i=0;i<=48;++i) {
            constexpr float pi=3.14159265358979323846F;
            const float angle=2.0F*pi*static_cast<float>(i)/48.0F;
            points.emplace_back(center.x+50.0F*std::cos(angle),
                                center.y+25.0F*std::sin(angle));
        }
        outline(d,points,o.osd,false);
    } else if (geometry=="point") {
        d->AddCircleFilled(center,7.0F,ink);
    } else if (geometry=="text") {
        d->AddText(img(570,195),ink,"VTR TEXT");
    }
    d->PopClipRect();
}
} // namespace

LegendPage drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path, bool selectPrimitives) {
    ImGui::TextUnformatted("VTR  /  Vision To Reg");
    ImGui::SameLine();
    ImGui::TextDisabled("  %zu выходов   |   hash: %s",
        catalog.outputs.size(), hashHex(catalog.hash).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Обновить TOML")) {
        try {
            auto next=readCatalog(path);
            catalog=std::move(next);
            error.clear();
        } catch(const std::exception& ex) {
            error=ex.what(); // Keep last valid catalogue visible.
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Перечитать локальный config/vtr.toml. SSH не требуется.");
    if (!error.empty())
        ImGui::TextColored({1.0F,0.43F,0.35F,1.0F},"Ошибка: %s",error.c_str());
    ImGui::Separator();

    // Gallery is a library of examples, NOT additional protocol output IDs.
    // Every sample uses the same Box structure and renderer as the TOML outputs.
    const auto primitives=[] {
        std::vector<Output> samples;
        auto add=[&](const char* name,Box box) {
            Output o; o.id=static_cast<std::uint16_t>(samples.size()+1);
            o.key=name; o.geometry="bbox"; o.box=std::move(box);
            samples.push_back(std::move(o));
        };
        Box base; base.stroke.color={0.30F,0.86F,0.78F};
        base.fill=base.stroke.color; base.stroke.width=2.0F;
        base.cornerWidth=4.0F; base.cornerPx=17.0F;
        Box s=base; add("Solid",s);
        s=base; s.stroke.pattern="dashed"; add("Dashed",s);
        s=base; s.stroke.pattern="dash_dot"; add("Dash-dot",s);
        s=base; s.stroke.pattern="none"; s.corners="solid"; add("Corners",s);
        s=base; s.stroke.pattern="dashed"; s.stroke.width=1.5F;
        s.corners="solid"; add("Corners + dashed",s);
        s=base; s.stroke.width=1.0F; s.corners="solid"; add("Corners + thin edges",s);
        s=base; s.stroke.pattern="none"; s.corners="solid"; s.cornerLayers=2;
        s.cornerWidth=2.0F; add("Double corners",s);
        s=base; s.stroke.width=1.5F; s.edgeMarkers="outside"; add("External markers",s);
        s=base; s.stroke.width=1.0F; s.edgeMarkers="center"; add("Mid-edge markers",s);
        s=base; s.frameLayers=2; s.stroke.width=1.5F; add("Double frame",s);
        s=base; s.stroke.pattern="none"; s.fillAlpha=0.24F; add("Fill only",s);
        s=base; s.stroke.pattern="none"; s.fillAlpha=0.16F; s.corners="solid";
        add("Fill + corners",s);
        s=base; s.diagonals="solid"; s.diagonalWidth=1.5F; add("Frame + diagonals",s);
        s=base; s.stroke.pattern="dashed"; s.diagonals="dashed";
        s.diagonalWidth=1.5F; add("Dashed + diagonals",s);
        s=base; s.roundingPx=9.0F; add("Rounded",s);
        return samples;
    };
    const auto gallery=[&](const std::vector<Output>& outputs,bool library) {
        if (ImGui::BeginChild("##vtr-scroll",ImVec2(0,0),ImGuiChildFlags_None)) {
            const int columns=ImGui::GetContentRegionAvail().x>=840.0F?2:1;
            if(ImGui::BeginTable("##vtr-grid",columns,ImGuiTableFlags_SizingStretchSame)) {
                for(const auto& o:outputs) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(o.id));
                    const bool stacked=ImGui::GetContentRegionAvail().x<390.0F;
                    constexpr float previewHeight=126.0F;
                    if(ImGui::BeginChild("##vtr-output",
                        {0,stacked?191.0F:146.0F},ImGuiChildFlags_Borders)) {
                        const float avail=ImGui::GetContentRegionAvail().x;
                        const float pw=stacked?avail:std::min(245.0F,avail*0.49F);
                        const auto p=ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##vtr-preview",{pw,previewHeight});
                        preview(ImGui::GetWindowDrawList(),o,p,{p.x+pw,p.y+previewHeight});
                        if(!stacked) ImGui::SameLine(0,12.0F);
                        ImGui::BeginGroup();
                        if(library) ImGui::TextUnformatted(o.key.c_str());
                        else {
                            ImGui::Text("#%u  %s",static_cast<unsigned>(o.id),o.key.c_str());
                            ImGui::TextColored({0.72F,0.78F,0.85F,1.0F},
                                "%s / %s",o.semantic.c_str(),o.geometry.c_str());
                            ImGui::TextDisabled("Details");
                            if(ImGui::IsItemHovered())
                                ImGui::SetTooltip("GT: %s\\nLabel: %s",o.benchmark.c_str(),
                                                 o.label.show?"ON":"OFF");
                        }
                        ImGui::EndGroup();
                    }
                    ImGui::EndChild(); ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    };
    LegendPage visible=LegendPage::None;
    if(ImGui::BeginTabBar("##vtr-gallery")) {
        if(ImGui::BeginTabItem("Outputs")) {
            visible=LegendPage::Outputs;
            gallery(catalog.outputs,false);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Primitives",nullptr,
            selectPrimitives?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None)) {
            static const auto samples=primitives();
            visible=LegendPage::Primitives;
            gallery(samples,true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    return visible;
}
} // namespace reg::vtr
