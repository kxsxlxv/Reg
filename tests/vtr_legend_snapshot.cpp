// Offline snapshot of the exact ImGui VTR legend renderer used by Reg.
// Uses SDL's software surface renderer, so no RTSP/Jetson/Vulkan/GPU needed.
#include "vtr/VtrCatalog.hpp"
#include "vtr/VtrLegend.hpp"
#include "vtr/VtrPhoto.hpp"
#include "render/ImGuiTheme.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>

int main(int argc,char** argv) {
    try {
        if (argc != 3 && argc != 4) {
            std::fprintf(stderr,"usage: vtr_legend_snapshot config/vtr.toml output.bmp [primitives|raster|editor]\n");
            return 2;
        }
        auto catalog=reg::vtr::readCatalog(argv[1]);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
            return 2;
        }
        constexpr int w=1100,h=1560;
        const bool primitives=argc==4 && std::string(argv[3])=="primitives";
        const bool raster=argc==4 && std::string(argv[3])=="raster";
        const bool editor=argc==4 && std::string(argv[3])=="editor";
        SDL_Surface* surface=SDL_CreateSurface(w,h,SDL_PIXELFORMAT_RGBA32);
        if(!surface) { std::fprintf(stderr,"SDL_CreateSurface: %s\n",SDL_GetError());return 2; }
        SDL_Renderer* renderer=SDL_CreateSoftwareRenderer(surface);
        if(!renderer) { std::fprintf(stderr,"SDL_CreateSoftwareRenderer: %s\n",SDL_GetError());return 2; }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io=ImGui::GetIO();
        io.IniFilename=nullptr;
        io.DisplaySize={static_cast<float>(w),static_cast<float>(h)};
        io.DisplayFramebufferScale={1.0F,1.0F};
        io.DeltaTime=1.0F/60.0F;
        reg::render::configureRegImGuiTheme(1.0F);
        reg::vtr::installPreviewPhoto(ImGui::GetIO().Fonts);
        reg::vtr::PreviewPhoto photo;
        if (!reg::vtr::previewPhoto(photo)) {
            std::fprintf(stderr,"VTR authentic photo atlas missing\\n");
            return 3;
        }
        if(!ImGui_ImplSDLRenderer3_Init(renderer)) {
            std::fprintf(stderr,"ImGui software backend init failed\n");
            return 2;
        }
        // ImGui tab selection is deferred until a subsequent frame:
        // SetSelected in BeginTabItem() cannot reliably show content in
        // the first layout pass. Warm up tabs before capturing frame four.
        bool legendVisible = false;
        auto visiblePage=reg::vtr::LegendPage::None;
        for (int frame = 0; frame < 4; ++frame) {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui::NewFrame();
            if(raster) {
                // Exercise actual ImGui/SDL raster output, not just vertices.
                // Use exact integer/half-pixel centers for each width.
                auto* draw=ImGui::GetForegroundDrawList();
                for(int n=1;n<=4;++n) {
                    reg::vtr::Box box;
                    box.stroke.color={0,1,0};
                    box.stroke.width=static_cast<float>(n);
                    const float off=n%2?0.5F:0.0F;
                    const float x=30.0F+100.0F*n+off,y=100.0F+off;
                    reg::vtr::drawBbox(draw,box,{x,y},{x+64,y+64});
                    box.stroke.pattern="none";box.corners="solid";
                    box.cornerWidth=static_cast<float>(n);box.cornerPx=16;
                    reg::vtr::drawBbox(draw,box,{x,y+100},{x+64,y+164});
                }
                // Red 50% underlay on a green frame/markers, including
                // their overlapping spans; then a mixed diagonal crossing.
                reg::vtr::Box box;
                box.stroke.color={0,1,0};box.stroke.width=2;
                box.outlineWidth=2;box.outlineColor={1,0,0};box.outlineAlpha=0.5F;
                box.edgeMarkers="center";box.markerWidth=4;box.markerPx=16;
                reg::vtr::drawBbox(draw,box,{500,300},{564,364});
                box.stroke.pattern="none";box.edgeMarkers="none";
                box.diagonals="solid";box.diagonalWidth=2;
                reg::vtr::drawBbox(draw,box,{600,300},{664,364});
                box.outlineAlpha=0;
                reg::vtr::drawBbox(draw,box,{700,300},{764,364});
                legendVisible=true;
            } else {
            ImGui::SetNextWindowPos({0,0},ImGuiCond_Always);
            ImGui::SetNextWindowSize({static_cast<float>(w),static_cast<float>(h)},
                                     ImGuiCond_Always);
            ImGui::Begin("Reg Telemetry",nullptr,
                ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
                ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::BeginTabBar("telemetry_tabs")) {
                if (ImGui::BeginTabItem("Обзор")) ImGui::EndTabItem();
                if (ImGui::BeginTabItem("Exact Sync")) ImGui::EndTabItem();
                if (ImGui::BeginTabItem("NetImgui")) ImGui::EndTabItem();
                if (ImGui::BeginTabItem("VTR / OSD",nullptr,
                                        ImGuiTabItemFlags_SetSelected)) {
                    legendVisible = true;
                    std::string error;
                    visiblePage=reg::vtr::drawLegend(catalog,error,argv[1],primitives,editor);
                    ImGui::EndTabItem();
                } else {
                    legendVisible = false;
                }
                if (ImGui::BeginTabItem("События")) ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
            ImGui::End();
            }
            ImGui::Render();
            ImDrawData* data=ImGui::GetDrawData();
            std::printf("VTR screenshot frame=%d legend=%d vertices=%d\n",
                frame,legendVisible?1:0,data!=nullptr?data->TotalVtxCount:0);
            if (!SDL_SetRenderDrawColor(renderer,18,22,30,255) ||
                !SDL_RenderClear(renderer)) {
                std::fprintf(stderr,"SDL renderer: %s\n",SDL_GetError());
                return 2;
            }
            ImGui_ImplSDLRenderer3_RenderDrawData(data,renderer);
            if (!SDL_RenderPresent(renderer)) {
                std::fprintf(stderr,"SDL_RenderPresent: %s\n",SDL_GetError());
                return 2;
            }
        }
        const auto expectedPage=editor?reg::vtr::LegendPage::Editor:(primitives?reg::vtr::LegendPage::Primitives:reg::vtr::LegendPage::Outputs);
        if (!legendVisible || (!raster && visiblePage!=expectedPage)) {
            std::fprintf(stderr,"VTR tab was never visible; rejecting blank screenshot\n");
            return 3;
        }
        // Count pixels differing from the normal ImGui background in the
        // interior, ignoring borders. A blank shot must fail CI, not pass.
        const auto* pixels=static_cast<const std::uint8_t*>(surface->pixels);
        if(raster) {
            // RGBA32 guarantees byte order R,G,B,A. Check the entire
            // reference stroke, not just a few sample pixels.
            const auto green=[&](int x,int y) {
                const auto* p=pixels+static_cast<std::size_t>(y)*surface->pitch+
                    static_cast<std::size_t>(x)*4;
                return p[1]>200&&p[0]<30&&p[2]<30;
            };
            for(int n=1;n<=4;++n) {
                const int x=30+100*n-(n/2),y=100-(n/2);
                const int outer=64+n;
                for(int row=-1;row<=outer;++row) for(int col=-1;col<=outer;++col) {
                    const bool within=col>=0&&col<outer&&row>=0&&row<outer;
                    const bool inside=col>=n&&col<64&&row>=n&&row<64;
                    const bool expected=within&&!inside;
                    if(green(x+col,y+row)!=expected) {
                        std::fprintf(stderr,"Raster frame width=%d x=%d y=%d expected=%d\n",
                            n,col,row,expected?1:0);
                        return 3;
                    }
                }
                // Check four corner caps and miter squares, away from
                // endpoint rounding differences for odd-width strokes.
                for(int row=0;row<outer;++row) for(int col=0;col<outer;++col) {
                    const bool left=col<n,right=col>=64,top=row<n,bottom=row>=64;
                    const bool nearX=col<15||col>=outer-15;
                    const bool nearY=row<15||row>=outer-15;
                    const bool required=((left||right)&&nearY)||((top||bottom)&&nearX);
                    if(required&&!green(x+col,y+100+row)) {
                        std::fprintf(stderr,"Raster L corner gap width=%d x=%d y=%d\n",n,col,row);
                        return 3;
                    }
                }
            }
            const auto checkColor=[&](int x,int y,int r,int g,int b) {
                const auto* p=pixels+static_cast<std::size_t>(y)*surface->pitch+
                    static_cast<std::size_t>(x)*4;
                if(std::abs(int(p[0])-r)>4||std::abs(int(p[1])-g)>4||
                   std::abs(int(p[2])-b)>4) {
                    std::fprintf(stderr,"Outline color/alpha at %d,%d: got %d,%d,%d expected %d,%d,%d\n",
                        x,y,int(p[0]),int(p[1]),int(p[2]),r,g,b);
                    return false;
                }
                return true;
            };
            // SDL clear is RGB(18,22,30); 50% red -> about (137,11,15).
            // These samples lie outside the green core, but in the underlay.
            if(!checkColor(515,297,137,11,15)||
               !checkColor(532,297,137,11,15)||
               !checkColor(635,332,137,11,15)||
               !checkColor(735,332,18,22,30)) return 3;
            std::printf("VTR pixel raster regression: widths 1..4 and outline alpha union PASS\n");
        }
        const auto* firstRow=pixels+static_cast<std::size_t>(3)*surface->pitch;
        std::uint32_t background=0;
        std::memcpy(&background,firstRow+static_cast<std::size_t>(3)*4,4);
        std::size_t varied=0;
        for (int y=70; y<h-2; y+=3) {
            for (int x=12; x<w-12; x+=3) {
                std::uint32_t pixel=0;
                std::memcpy(&pixel,pixels+static_cast<std::size_t>(y)*surface->pitch+
                            static_cast<std::size_t>(x)*4,4);
                if (pixel!=background) ++varied;
            }
        }
        std::printf("VTR screenshot pixel diversity=%zu (sampled)\n",varied);
        if (!raster && varied<1500) {
            std::fprintf(stderr,"VTR screenshot blank; failing visual smoke test\n");
            return 3;
        }
        if(!SDL_SaveBMP(surface,argv[2])) {
            std::fprintf(stderr,"SDL_SaveBMP: %s\n",SDL_GetError());
            return 2;
        }
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
        SDL_Quit();
        std::printf("Saved offline VTR legend: %s\n",argv[2]);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"vtr_legend_snapshot: %s\n",e.what());
        return 1;
    }
}
