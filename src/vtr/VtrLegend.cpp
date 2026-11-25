#include "vtr/VtrLegend.hpp"
#include "vtr/VtrDraw.hpp"
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
using draw::ellipsePath;
using draw::filledPath;
using draw::outline;
using draw::paintLine;
using draw::rectangle;
using draw::rgba;
using draw::stroke;
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
void preview(ImDrawList* d,const Output& sample,ImVec2 min,ImVec2 max,
             int background=0,float zoom=1.0F,const std::vector<Output>* layers=nullptr) {
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
    // Common safe area for ALL cards, including labels and external markers.
    // The reference photo/annotation stay identical between styles.
    const float sidePad=std::min(12.0F,width*0.12F);
    const float topPad=std::min(26.0F,height*0.22F);
    const float bottomPad=std::min(12.0F,height*0.12F);
    const float scale=zoom*std::max(0.01F,std::min((width-2*sidePad)/srcW,
                              (height-topPad-bottomPad)/srcH));
    const ImVec2 photoMin{min.x+(width-srcW*scale)*0.5F,
        min.y+topPad+(height-topPad-bottomPad-srcH*scale)*0.5F};
    const ImVec2 photoMax{photoMin.x+srcW*scale,photoMin.y+srcH*scale};
    if(background!=0) {
        d->AddRectFilled(photoMin,photoMax,background==1?IM_COL32(225,230,235,255):IM_COL32(30,37,47,255));
    } else if (hasPhoto) {
        d->AddImage(photo.texture,photoMin,photoMax,photo.uv0,photo.uv1,
                    IM_COL32(255,255,255,255));
    } else {
        d->AddText({min.x+8,min.y+8},IM_COL32(255,115,100,255),
                   "Фото недоступно");
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
    const auto drawLayer=[&](const Output& o) {
    const ImU32 ink=rgba(o.osd.stroke.color,o.osd.alpha);
    const auto& geometry=o.geometry;
    if (geometry=="bbox") {
        const bool roi=o.semantic=="search_region";
        const ImVec2 lo=roi?img(50,10):a;
        const ImVec2 hi=roi?img(1191,735):b;
        drawBbox(d,o.box,lo,hi);
        draw::drawLabel(d,o,lo,hi,min,max,sampleLabel(o.label.text));
    } else if (geometry=="line") {
        outline(d,{img(155,660),img(1105,65)},o.osd,false);
    } else if (geometry=="arrow") {
        const ImVec2 p=img(630,290),q=img(915,460);
        const float len=std::hypot(q.x-p.x,q.y-p.y);
        if(len>0.01F&&o.osd.stroke.pattern!="none") {
            const float head=std::min(o.osd.arrowHeadPx,len*0.45F);
            const float ux=(q.x-p.x)/len,uy=(q.y-p.y)/len;
            const ImVec2 left{q.x-ux*head-uy*head*0.45F,q.y-uy*head+ux*head*0.45F};
            const ImVec2 right{q.x-ux*head+uy*head*0.45F,q.y-uy*head-ux*head*0.45F};
            outline(d,{p,q},o.osd,false);
            Osd cap=o.osd;cap.stroke.pattern="solid";
            outline(d,{left,q,right},cap,false);
        }
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
            const float alpha=geometry=="mask"?o.osd.alpha:o.osd.fillAlpha;
            d->AddConvexPolyFilled(points,5,rgba(geometry=="mask"?o.osd.stroke.color:o.osd.fill.value_or(o.osd.stroke.color),alpha));
            if (geometry=="polygon")
                outline(d,{points[0],points[1],points[2],points[3],
                           points[4]},o.osd,true);
        }
    } else if (geometry=="circle"||geometry=="ellipse") {
        const auto points=geometry=="circle"?ellipsePath(center,30,30):ellipsePath(center,50,25);
        if(o.osd.fill) filledPath(d,points,o.osd);
        outline(d,points,o.osd,true);
    } else if (geometry=="point") {
        if(o.osd.outlineWidth>0)
            d->AddCircleFilled(center,o.osd.pointRadiusPx+o.osd.outlineWidth,
                rgba(o.osd.outlineColor,o.osd.outlineAlpha));
        d->AddCircleFilled(center,o.osd.pointRadiusPx,ink);
    } else if (geometry=="text") {
        d->AddText(img(570,195),ink,"ТЕКСТ OSD");
    }
    };
    if(layers) {
        std::vector<const Output*> ordered;
        for(const auto& o:*layers) if(o.visible) ordered.push_back(&o);
        std::stable_sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->zOrder<b->zOrder;});
        for(const auto* o:ordered) drawLayer(*o);
    } else drawLayer(sample);
    d->PopClipRect();
}
} // namespace

void drawBbox(ImDrawList* draw,const Box& style,ImVec2 min,ImVec2 max) {
    rectangle(draw,style,min,max);
}

LegendPage drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path, bool selectPrimitives, bool selectEditor) {
    // Draft is deliberately independent of the active catalogue. No changes
    // reach disk or Outputs until Save succeeds. A reload invalidates drafts.
    struct EditorState {
        std::filesystem::path path;
        std::uint64_t hash{};
        int selected{-1};
        int pending{-1};
        Output draft;
        bool dirty{},combined{};
        int background{};
        float zoom{1};
        std::string status;
    };
    static EditorState editor;
    if(editor.path!=path||editor.hash!=catalog.hash) {
        editor=EditorState{};editor.path=path;editor.hash=catalog.hash;
    }
    ImGui::TextUnformatted("OSD");
    ImGui::SameLine();
    ImGui::TextDisabled("  %zu выходов   |   хеш: %s",
        catalog.outputs.size(), hashHex(catalog.hash).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(editor.dirty);
    if (ImGui::Button("Обновить TOML")) {
        try {
            auto next=readCatalog(path);
            catalog=std::move(next);
            error.clear();
        } catch(const std::exception& ex) {
            error=ex.what(); // Keep last valid catalogue visible.
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(editor.dirty?"Сначала сохраните или отмените правки в редакторе.":"Перечитать локальный config/vtr.toml.");
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
        Box s=base; add("Сплошная",s);
        s=base; s.stroke.pattern="dashed"; add("Пунктир",s);
        s=base; s.stroke.pattern="dash_dot"; add("Штрихпунктир",s);
        s=base; s.stroke.pattern="none"; s.corners="solid"; add("Углы",s);
        s=base; s.stroke.pattern="dashed"; s.stroke.width=1.5F;
        s.corners="solid"; add("Углы + пунктир",s);
        s=base; s.stroke.width=1.0F; s.corners="solid"; add("Углы + тонкие грани",s);
        s=base; s.stroke.pattern="none"; s.corners="solid"; s.cornerLayers=2;
        s.cornerWidth=2.0F; add("Двойные углы",s);
        s=base; s.stroke.width=1.5F; s.edgeMarkers="outside"; add("Внешние маркеры",s);
        s=base; s.stroke.width=1.0F; s.edgeMarkers="center"; s.markerPx=18.0F; s.markerWidth=4.0F; add("Маркеры по центру",s);
        s=base; s.frameLayers=2; s.stroke.width=1.5F; add("Двойная рамка",s);
        s=base; s.stroke.pattern="none"; s.fillAlpha=0.24F; add("Только заливка",s);
        s=base; s.stroke.pattern="none"; s.fillAlpha=0.16F; s.corners="solid";
        add("Заливка + углы",s);
        s=base; s.diagonals="solid"; s.diagonalWidth=1.5F; add("Рамка + диагонали",s);
        s=base; s.stroke.pattern="dashed"; s.diagonals="dashed";
        s.diagonalWidth=1.5F; add("Пунктир + диагонали",s);
        s=base; s.roundingPx=9.0F; add("Скруглённая",s);
        for(const char* kind:{"line","arrow","polyline","polygon","circle","ellipse","point","text","mask","heatmap"}) {
            Output o;o.id=static_cast<std::uint16_t>(samples.size()+1);o.key=kind;o.geometry=kind;
            o.osd.stroke.color=base.stroke.color;o.osd.stroke.width=2;o.osd.alpha=1;
            if(o.geometry=="mask") {o.semantic="sky";o.osd.alpha=0.3F;}
            if(o.geometry=="heatmap") o.osd.alpha=0.8F;
            samples.push_back(std::move(o));
        }
        return samples;
    };
    static const auto samples=primitives();
    const auto localized=[](std::string_view value)->const char* {
        if(value=="none") return "Нет";
        if(value=="solid") return "Сплошная";
        if(value=="dashed") return "Пунктир";
        if(value=="dash_dot") return "Штрихпунктир";
        if(value=="center") return "По центру";
        if(value=="outside") return "Снаружи";
        if(value=="top_left_outside") return "Сверху слева";
        if(value=="bottom_left_outside") return "Снизу слева";
        if(value=="line") return "Линия";
        if(value=="arrow") return "Стрелка";
        if(value=="polyline") return "Полилиния";
        if(value=="polygon") return "Полигон";
        if(value=="circle") return "Окружность";
        if(value=="ellipse") return "Эллипс";
        if(value=="point") return "Точка";
        if(value=="text") return "Текст";
        if(value=="mask") return "Маска";
        if(value=="heatmap") return "Тепловая карта";
        if(value=="bbox") return "BBOX";
        if(value=="detection") return "Детекция";
        if(value=="tracking") return "Трекинг";
        if(value=="propagation") return "Пропагация";
        if(value=="prediction") return "Прогноз";
        if(value=="sky") return "Небо";
        if(value=="motion") return "Движение";
        if(value=="contour") return "Контур";
        if(value=="rejected") return "Отклонено";
        if(value=="search_region") return "Область поиска";
        return nullptr;
    };
    const auto gallery=[&](const std::vector<Output>& outputs,bool library) {
        if (ImGui::BeginChild("##vtr-scroll",ImVec2(0,0),ImGuiChildFlags_None)) {
            const int columns=ImGui::GetContentRegionAvail().x>=840.0F?2:1;
            if(ImGui::BeginTable("##vtr-grid",columns,ImGuiTableFlags_SizingStretchSame)) {
                for(const auto& o:outputs) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(o.id));
                    const bool stacked=ImGui::GetContentRegionAvail().x<390.0F;
                    constexpr float previewHeight=152.0F;
                    if(ImGui::BeginChild("##vtr-output",
                        {0,stacked?217.0F:172.0F},ImGuiChildFlags_Borders)) {
                        const float avail=ImGui::GetContentRegionAvail().x;
                        const float pw=stacked?avail:std::min(245.0F,avail*0.49F);
                        const auto p=ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##vtr-preview",{pw,previewHeight});
                        preview(ImGui::GetWindowDrawList(),o,p,{p.x+pw,p.y+previewHeight});
                        if(!stacked) ImGui::SameLine(0,12.0F);
                        ImGui::BeginGroup();
                        if(library) ImGui::TextUnformatted(localized(o.key)?localized(o.key):o.key.c_str());
                        else {
                            ImGui::Text("#%u  %s",static_cast<unsigned>(o.id),o.key.c_str());
                            ImGui::TextColored({0.72F,0.78F,0.85F,1.0F},
                                "%s / %s",localized(o.semantic)?localized(o.semantic):o.semantic.c_str(),
                                localized(o.geometry)?localized(o.geometry):o.geometry.c_str());
                            if(!o.visible) ImGui::TextDisabled("Скрыт в общих слоях");
                            ImGui::TextDisabled("Подробности");
                            if(ImGui::IsItemHovered())
                                ImGui::SetTooltip("GT: %s\nЛейбл: %s",o.benchmark.c_str(),
                                                 o.label.show?"Вкл.":"Выкл.");
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
    auto editorPage=[&]() {
        if(editor.path!=path||editor.hash!=catalog.hash) {
            editor=EditorState{};editor.path=path;editor.hash=catalog.hash;
        }
        if(catalog.outputs.empty()) { ImGui::TextUnformatted("Сначала загрузите каталог OSD.");return; }
        auto select=[&](int index) {
            editor.selected=index;editor.draft=catalog.outputs[static_cast<std::size_t>(index)];
            editor.dirty=false;editor.status.clear();
        };
        if(editor.selected<0||editor.selected>=static_cast<int>(catalog.outputs.size())) select(0);
        ImGui::SetNextItemWidth(std::min(340.0F,ImGui::GetContentRegionAvail().x));
        if(ImGui::BeginCombo("Выход",editor.draft.key.c_str())) {
            for(int i=0;i<static_cast<int>(catalog.outputs.size());++i) {
                if(ImGui::Selectable(catalog.outputs[static_cast<std::size_t>(i)].key.c_str(),i==editor.selected)
                    &&i!=editor.selected) {
                    if(editor.dirty) editor.pending=i;
                    else select(i);
                }
            }
            ImGui::EndCombo();
        }
        auto& o=editor.draft;
        auto save=[&]() {
            try {
                catalog=saveOutputStyle(path,editor.hash,o);
                editor.hash=catalog.hash;
                editor.draft=catalog.outputs[static_cast<std::size_t>(editor.selected)];
                editor.dirty=false;editor.status="Сохранено. Копия: vtr.toml.bak";error.clear();
                return true;
            } catch(const std::exception& ex) {editor.status=ex.what();return false;}
        };
        if(editor.pending>=0&&!ImGui::IsPopupOpen("Несохранённые изменения###osd-switch"))
            ImGui::OpenPopup("Несохранённые изменения###osd-switch");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{0.5F,0.5F});
        if(ImGui::BeginPopupModal("Несохранённые изменения###osd-switch",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Есть несохранённые изменения: %s",o.key.c_str());
            ImGui::TextUnformatted("Сохранить перед переключением?");
            if(!editor.status.empty()) ImGui::TextWrapped("%s",editor.status.c_str());
            if(ImGui::Button("Сохранить")) {
                if(save()) {const int next=editor.pending;editor.pending=-1;select(next);ImGui::CloseCurrentPopup();}
            }
            ImGui::SameLine();
            if(ImGui::Button("Не сохранять")) {
                const int next=editor.pending;editor.pending=-1;select(next);ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if(ImGui::Button("Отмена")) {editor.pending=-1;ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        ImGui::BeginDisabled(!editor.dirty);
        if(ImGui::Button("Сохранить TOML")) save();
        ImGui::SameLine();
        if(ImGui::Button("Отменить правки")) select(editor.selected);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Заводской стиль")) {
            try {
                const auto factory=factoryCatalog();
                const auto found=std::find_if(factory.outputs.begin(),factory.outputs.end(),[&](const auto& f){
                    return f.id==o.id&&f.key==o.key&&f.geometry==o.geometry;
                });
                if(found==factory.outputs.end()) editor.status="Для пользовательского выхода нет заводского стиля.";
                else {
                    o.box=found->box;o.osd=found->osd;o.label=found->label;
                    o.visible=found->visible;o.zOrder=found->zOrder;
                    editor.dirty=true;editor.status="Заводской стиль загружен. Сохраните для применения.";
                }
            } catch(const std::exception& ex) {editor.status=ex.what();}
        }
        if(editor.dirty) {ImGui::SameLine();ImGui::TextDisabled("Не сохранено");}
        if(!editor.status.empty()) ImGui::TextWrapped("%s",editor.status.c_str());
        ImGui::Separator();
        // Preview uses the full panel width, controls scroll below it.
        auto previewPane=[&] {
            ImGui::TextUnformatted("Предпросмотр");
            ImGui::SetNextItemWidth(120);
            ImGui::Combo("Фон",&editor.background,"Фото\0Светлый\0Тёмный\0");
            ImGui::SetNextItemWidth(std::min(240.0F,ImGui::GetContentRegionAvail().x));
            ImGui::SliderFloat("Масштаб объекта",&editor.zoom,0.08F,1.0F,"%.2fx");
            ImGui::Checkbox("Все слои",&editor.combined);
            const float pw=ImGui::GetContentRegionAvail().x;
            const float ph=std::max(150.0F,std::min(pw*0.57F+38.0F,ImGui::GetContentRegionAvail().y*0.60F));
            const auto p=ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##live",{pw,ph});
            std::vector<Output> layers;
            if(editor.combined) {
                layers=catalog.outputs;
                layers[static_cast<std::size_t>(editor.selected)]=o;
            }
            preview(ImGui::GetWindowDrawList(),o,p,{p.x+pw,p.y+ph},
                    editor.background,editor.zoom,editor.combined?&layers:nullptr);
            ImGui::TextDisabled("Демонстрация OSD");
            if(o.geometry=="bbox") {
                ImGui::TextUnformatted("Малые BBOX (px)");
                const auto a=ImGui::GetCursorScreenPos();
                const float available=ImGui::GetContentRegionAvail().x;
                ImGui::Dummy({available,112});
                auto* d=ImGui::GetWindowDrawList();
                d->AddRectFilled(a,{a.x+available,a.y+112},IM_COL32(54,71,85,255));
                const float sizes[]={16,32,64};
                for(int i=0;i<3;++i) {
                    const float x=a.x+available*(i+0.5F)/3,y=a.y+48;
                    const float s=sizes[i]*0.5F;
                    d->PushClipRect({a.x+available*i/3,a.y},{a.x+available*(i+1)/3,a.y+112},true);
                    drawBbox(d,o.box,{x-s,y-s},{x+s,y+s});
                    const auto label=std::to_string(static_cast<int>(sizes[i]))+" px";
                    d->AddText({x-ImGui::CalcTextSize(label.c_str()).x/2,a.y+93},IM_COL32_WHITE,label.c_str());
                    d->PopClipRect();
                }
            }
        };
        auto controls=[&] {
            auto edit=[&](bool changed){editor.dirty|=changed;};
            auto slider=[&](const char* name,float& v,float lo,float hi,const char* fmt="%.1f") {
                edit(ImGui::SliderFloat(name,&v,lo,hi,fmt));
            };
            auto colorEdit=[&](const char* name,Color& color) {
                float rgb[]={color.r,color.g,color.b};
                if(ImGui::ColorEdit3(name,rgb,ImGuiColorEditFlags_NoInputs)) {
                    color={rgb[0],rgb[1],rgb[2]};editor.dirty=true;
                }
            };
            auto choose=[&](const char* name,std::string& value,const std::vector<const char*>& options) {
                if(ImGui::BeginCombo(name,localized(value)?localized(value):value.c_str())) {
                    for(const char* v:options) {
                        ImGui::PushID(v);
                        if(ImGui::Selectable(localized(v)?localized(v):v,value==v)) {value=v;editor.dirty=true;}
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            };
            auto pattern=[&](const char* name,std::string& value) {choose(name,value,{"none","solid","dashed","dash_dot"});};
            auto optionalColor=[&](const char* name,std::optional<Color>& color,Color inherited) {
                ImGui::PushID(name);
                bool custom=color.has_value();
                if(ImGui::Checkbox("Свой цвет",&custom)) {
                    if(custom) color=inherited; else color.reset();
                    editor.dirty=true;
                }
                if(color) colorEdit(name,*color);
                ImGui::PopID();
            };
            edit(ImGui::Checkbox("Видимость",&o.visible));
            edit(ImGui::SliderInt("Порядок слоёв",&o.zOrder,-1000,1000));
            if(o.geometry!="bbox") {
                colorEdit("Цвет",o.osd.stroke.color);
                slider("Непрозрачность",o.osd.alpha,0,1,"%.2f");
                slider("Толщина",o.osd.stroke.width,0.5F,16);
                pattern("Тип линии",o.osd.stroke.pattern);
                if(o.geometry=="line"||o.geometry=="arrow"||o.geometry=="polyline"||
                   o.geometry=="polygon"||o.geometry=="circle"||o.geometry=="ellipse") {
                    slider("Штрих, px (0 = авто)",o.osd.dashPx,0,64);
                    slider("Интервал, px (0 = авто)",o.osd.gapPx,0,32);
                    slider("Точка, px (0 = авто)",o.osd.dotPx,0,16);
                }
                if(o.geometry!="mask"&&o.geometry!="heatmap"&&o.geometry!="text") {
                    colorEdit("Цвет обводки",o.osd.outlineColor);
                    slider("Толщина обводки",o.osd.outlineWidth,0,8);
                    slider("Непрозрачность обводки",o.osd.outlineAlpha,0,1,"%.2f");
                }
                if(o.geometry=="polygon"||o.geometry=="circle"||o.geometry=="ellipse") {
                    optionalColor("Цвет заливки",o.osd.fill,o.osd.stroke.color);
                    slider("Непрозрачность заливки",o.osd.fillAlpha,0,1,"%.2f");
                }
                if(o.geometry=="point") slider("Радиус точки",o.osd.pointRadiusPx,1,64);
                if(o.geometry=="arrow") slider("Наконечник стрелки",o.osd.arrowHeadPx,2,64);
                return;
            }
            auto& b=o.box;
            if(ImGui::BeginCombo("Шаблон","Выбрать геометрию BBOX")) {
                for(const auto& p:samples) if(p.geometry=="bbox"&&ImGui::Selectable(p.key.c_str())) {
                    const Box old=b;
                    b=p.box;b.stroke.color=old.stroke.color;b.fill=old.fill;
                    b.outlineColor=old.outlineColor;b.outlineWidth=old.outlineWidth;
                    b.outlineAlpha=old.outlineAlpha;b.adaptive=old.adaptive;
                    b.edgeAlpha=old.edgeAlpha;b.cornerAlpha=old.cornerAlpha;b.markerAlpha=old.markerAlpha;
                    b.cornerColor=old.cornerColor;b.markerColor=old.markerColor;
                    b.dashPx=old.dashPx;b.gapPx=old.gapPx;b.dotPx=old.dotPx;
                    editor.dirty=true;
                }
                ImGui::EndCombo();
            }
            edit(ImGui::Checkbox("Адаптация малых BBOX",&b.adaptive));
            if(ImGui::CollapsingHeader("Обводка",ImGuiTreeNodeFlags_DefaultOpen)) {
                colorEdit("Цвет обводки",b.outlineColor);
                slider("Толщина обводки",b.outlineWidth,0,8);
                slider("Непрозрачность обводки",b.outlineAlpha,0,1,"%.2f");
            }
            if(ImGui::CollapsingHeader("Грани и пунктир",ImGuiTreeNodeFlags_DefaultOpen)) {
                colorEdit("Цвет граней",b.stroke.color);pattern("Грани",b.stroke.pattern);
                slider("Толщина граней",b.stroke.width,0.5F,16);
                slider("Непрозрачность граней",b.edgeAlpha,0,1,"%.2f");
                slider("Штрих, px (0 = авто)",b.dashPx,0,64);
                slider("Интервал, px (0 = авто)",b.gapPx,0,32);
                slider("Точка, px (0 = авто)",b.dotPx,0,16);
            }
            if(ImGui::CollapsingHeader("Углы и контуры")) {
                pattern("Углы",b.corners);optionalColor("Цвет углов",b.cornerColor,b.stroke.color);
                slider("Толщина углов",b.cornerWidth,0.5F,16);slider("Длина углов",b.cornerPx,1,200);
                slider("Непрозрачность углов",b.cornerAlpha,0,1,"%.2f");
                edit(ImGui::SliderInt("Слои углов",&b.cornerLayers,1,2));
                edit(ImGui::SliderInt("Слои рамки",&b.frameLayers,1,2));
                slider("Интервал слоёв",b.layerGapPx,1,24);
                slider("Скругление",b.roundingPx,0,100);
            }
            if(ImGui::CollapsingHeader("Маркеры, диагонали, заливка")) {
                choose("Маркеры",b.edgeMarkers,{"none","center","outside"});
                optionalColor("Цвет маркеров",b.markerColor,b.stroke.color);
                slider("Толщина маркеров",b.markerWidth,0.5F,16);slider("Длина маркеров",b.markerPx,1,200);
                slider("Непрозрачность маркеров",b.markerAlpha,0,1,"%.2f");
                pattern("Диагонали",b.diagonals);slider("Толщина диагоналей",b.diagonalWidth,0.5F,16);
                colorEdit("Цвет заливки",b.fill);slider("Непрозрачность заливки",b.fillAlpha,0,1,"%.2f");
            }
            if(ImGui::CollapsingHeader("Лейбл")) {
                auto& a=o.label;
                edit(ImGui::Checkbox("Показать лейбл",&a.show));
                std::array<char,129> buf{};
                std::snprintf(buf.data(),buf.size(),"%s",a.text.c_str());
                if(ImGui::InputText("Шаблон текста",buf.data(),buf.size())) {a.text=buf.data();editor.dirty=true;}
                choose("Положение",a.place,{"top_left_outside","bottom_left_outside"});
                edit(ImGui::Checkbox("Автоконтраст текста",&a.autoColor));
                if(!a.autoColor) colorEdit("Цвет текста",a.color);
                edit(ImGui::Checkbox("Фон цвета BBOX",&a.boxBackground));
                if(!a.boxBackground) colorEdit("Цвет фона",a.background);
                slider("Непрозрачность фона",a.backgroundAlpha,0,1,"%.2f");
                slider("Шрифт, px",a.fontPx,8,48);
                slider("Поля, px",a.paddingPx,0,24);
                slider("Отступ лейбла",a.gapPx,0,24);
                slider("Скругление лейбла",a.roundingPx,0,24);
                slider("Макс. ширина (0 = авто)",a.maxWidthPx,0,2048,"%.0f");
            }
        };
        if(ImGui::BeginChild("##editor-scroll",{0,0})) {
            previewPane();
            ImGui::Separator();
            ImGui::PushID(static_cast<int>(o.id));
            ImGui::SetNextItemWidth(-1);
            controls();
            ImGui::PopID();
        }
        ImGui::EndChild();
    };
    LegendPage visible=LegendPage::None;
    if(ImGui::BeginTabBar("##vtr-gallery")) {
        if(ImGui::BeginTabItem("Выходы###Outputs")) {
            visible=LegendPage::Outputs;
            gallery(catalog.outputs,false);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Примитивы###Primitives",nullptr,
            selectPrimitives?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None)) {
            visible=LegendPage::Primitives;
            gallery(samples,true);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Редактор###Editor",nullptr,
            selectEditor?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None)) {
            visible=LegendPage::Editor;
            editorPage();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    return visible;
}
} // namespace reg::vtr
