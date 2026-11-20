#include "vtr/VtrLegend.hpp"
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
void stroke(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 color,
            float width, std::string_view pattern) {
    if (pattern == "none") return;
    if (pattern == "solid") { d->AddLine(a,b,color,width); return; }
    const float dx=b.x-a.x,dy=b.y-a.y,len=std::sqrt(dx*dx+dy*dy);
    if (len < 0.01F) return;
    const float dash=7.0F, gap=5.0F;
    for (float s=0;s<len;s+=dash+gap) {
        const float t=std::min(len,s+dash);
        d->AddLine(ImVec2(a.x+dx*s/len,a.y+dy*s/len),
                   ImVec2(a.x+dx*t/len,a.y+dy*t/len),color,width);
    }
}
void drone(ImDrawList* d, ImVec2 center) {
    const ImU32 body=IM_COL32(83,104,124,255);
    const ImU32 edge=IM_COL32(128,151,169,255);
    // Small top-down UAV silhouette; never a fetched or live video frame.
    d->AddLine(ImVec2(center.x-37,center.y-20),
               ImVec2(center.x+37,center.y+20),body,9);
    d->AddLine(ImVec2(center.x+37,center.y-20),
               ImVec2(center.x-37,center.y+20),body,9);
    for (int dx : {-39,39}) for (int dy : {-22,22}) {
        d->AddCircleFilled(ImVec2(center.x+dx,center.y+dy),8,IM_COL32(48,67,86,255));
        d->AddCircle(ImVec2(center.x+dx,center.y+dy),8,edge,0,1);
    }
    const ImVec2 p[]={
        {center.x,center.y-24},{center.x+11,center.y+7},
        {center.x+5,center.y+20},{center.x-5,center.y+20},
        {center.x-11,center.y+7}
    };
    d->AddConvexPolyFilled(p,5,body);
    d->AddLine(p[0],p[1],edge,1.5F);
    d->AddLine(p[0],p[4],edge,1.5F);
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
void rectangle(ImDrawList* d, const Box& s, ImVec2 a, ImVec2 b) {
    const ImU32 line=rgba(s.stroke.color);
    if (s.fillAlpha > 0.0F)
        d->AddRectFilled(a,b,rgba(s.fill,s.fillAlpha));
    const float w=std::max(0.0F,b.x-a.x),h=std::max(0.0F,b.y-a.y);
    const float k=std::min({s.cornerPx,w*0.45F,h*0.45F});
    const bool corners=s.corners!="none";
    const float m=corners?k:0.0F;
    const std::array<std::pair<ImVec2,ImVec2>,4> edges{{
        {{a.x+m,a.y},{b.x-m,a.y}},{{b.x,a.y+m},{b.x,b.y-m}},
        {{b.x-m,b.y},{a.x+m,b.y}},{{a.x,b.y-m},{a.x,a.y+m}}
    }};
    for (const auto& [p,q] : edges)
        stroke(d,p,q,line,s.stroke.width,s.stroke.pattern);
    if (corners) {
        const ImVec2 c[4]={{a.x,a.y},{b.x,a.y},{b.x,b.y},{a.x,b.y}};
        const float sx[4]={1,-1,-1,1},sy[4]={1,1,-1,-1};
        for (int n=0;n<4;++n) {
            stroke(d,c[n],{c[n].x+sx[n]*k,c[n].y},line,
                   s.stroke.width,s.corners);
            stroke(d,c[n],{c[n].x,c[n].y+sy[n]*k},line,
                   s.stroke.width,s.corners);
        }
    }
    stroke(d,a,b,line,s.stroke.width,s.diagonals);
    stroke(d,{b.x,a.y},{a.x,b.y},line,s.stroke.width,s.diagonals);
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
    const ImVec2 center{(min.x+max.x)*0.5F,min.y+(max.y-min.y)*0.52F};
    drone(d,center);
    const float x=center.x-59.0F,y=center.y-35.0F;
    const ImVec2 a{x,y},b{x+118.0F,y+73.0F};
    const ImU32 ink=rgba(o.osd.stroke.color,o.osd.alpha);
    const auto& geometry=o.geometry;
    if (geometry=="bbox") {
        rectangle(d,o.box,a,b);
        if (o.label.show && !o.label.text.empty()) {
            std::string text=sampleLabel(o.label.text);
            const ImVec2 textSize=ImGui::CalcTextSize(text.c_str());
            const float pad=o.label.paddingPx;
            const float totalHeight=textSize.y+2.0F*pad;
            const float ly=o.label.place=="bottom_left_outside"
                ? b.y+o.label.gapPx : a.y-totalHeight-o.label.gapPx;
            const float safeY=std::clamp(ly,min.y+2.0F,max.y-totalHeight-2.0F);
            d->AddRectFilled({a.x,safeY},
                {a.x+textSize.x+2.0F*pad,safeY+totalHeight},
                rgba(o.label.background,o.label.backgroundAlpha),3.0F);
            d->AddText({a.x+pad,safeY+pad},rgba(o.label.color),text.c_str());
        }
    } else if (geometry=="line") {
        stroke(d,a,b,ink,o.osd.stroke.width,o.osd.stroke.pattern);
    } else if (geometry=="arrow") {
        stroke(d,{a.x,a.y+65},{b.x,b.y-65},ink,o.osd.stroke.width,o.osd.stroke.pattern);
        d->AddTriangleFilled({b.x,b.y-65},{b.x-13,b.y-72},{b.x-10,b.y-57},ink);
    } else if (geometry=="polyline") {
        outline(d,{{a.x,a.y+48},{a.x+20,a.y+18},{a.x+44,a.y+39},
                   {a.x+75,a.y+11},{b.x,b.y-30}},o.osd,false);
    } else if (geometry=="polygon" || geometry=="mask" || geometry=="heatmap") {
        const ImVec2 points[]={{a.x+10,a.y+50},{a.x+30,a.y+8},
                               {b.x-20,a.y+16},{b.x-5,b.y-5},
                               {a.x+27,b.y}};
        if (geometry=="heatmap") {
            for (int i=4;i>=1;--i)
                d->AddCircleFilled({center.x+16,center.y},15.0F*i,
                    rgba(o.osd.stroke.color,o.osd.alpha*0.13F));
        } else {
            const float alpha=geometry=="mask"?o.osd.alpha:0.22F;
            d->AddConvexPolyFilled(points,5,rgba(o.osd.stroke.color,alpha));
            if(geometry=="polygon")
                outline(d,{points[0],points[1],points[2],points[3],points[4]},o.osd,true);
        }
    } else if (geometry=="circle") {
        d->AddCircle(center,37.0F,ink,0,o.osd.stroke.width);
    } else if (geometry=="ellipse") {
        std::vector<ImVec2> points;
        for (int i=0;i<=48;++i) {
            constexpr float pi=3.14159265358979323846F;
            float angle=2.0F*pi*static_cast<float>(i)/48.0F;
            points.emplace_back(center.x+56.0F*std::cos(angle),
                                center.y+31.0F*std::sin(angle));
        }
        outline(d,points,o.osd,false);
    } else if (geometry=="point") {
        d->AddCircleFilled(center,7.0F,ink);
    } else if (geometry=="text") {
        d->AddText({a.x+20,a.y+14},ink,"VTR TEXT");
    }
    d->PopClipRect();
}
} // namespace

void drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path) {
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

    // A compact responsive gallery. The previous one-column layout wasted
    // the entire right-hand side of the Telemetry window.
    if (ImGui::BeginChild("##vtr-scroll",ImVec2(0.0F,0.0F),ImGuiChildFlags_None)) {
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const int columns = contentWidth >= 840.0F ? 2 : 1;
        if (ImGui::BeginTable("##vtr-grid",columns,
                              ImGuiTableFlags_SizingStretchSame)) {
            for (const auto& o : catalog.outputs) {
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(o.id));
                const float cellWidth=ImGui::GetContentRegionAvail().x;
                const bool stacked=cellWidth < 390.0F;
                if (ImGui::BeginChild("##vtr-output",
                        ImVec2(0.0F,stacked ? 178.0F : 132.0F),
                        ImGuiChildFlags_Borders)) {
                    const float available=ImGui::GetContentRegionAvail().x;
                    const float previewWidth=stacked
                        ? available : std::min(245.0F,available*0.49F);
                    const ImVec2 p=ImGui::GetCursorScreenPos();
                    constexpr float previewHeight=112.0F;
                    ImGui::InvisibleButton("##vtr-preview",
                                           ImVec2(previewWidth,previewHeight));
                    preview(ImGui::GetWindowDrawList(),o,p,
                            {p.x+previewWidth,p.y+previewHeight});
                    if (!stacked)
                        ImGui::SameLine(0.0F,12.0F);
                    ImGui::BeginGroup();
                    ImGui::Text("#%u  %s",
                        static_cast<unsigned>(o.id),o.key.c_str());
                    ImGui::TextColored({0.72F,0.78F,0.85F,1.0F},
                        "%s  /  %s",o.semantic.c_str(),o.geometry.c_str());
                    ImGui::TextDisabled("GT: %s",o.benchmark.c_str());
                    ImGui::TextDisabled("Label: %s",
                                        o.label.show?"ON":"OFF");
                    ImGui::EndGroup();
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}
} // namespace reg::vtr
