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
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>

int main(int argc,char** argv) {
    try {
        if (argc != 3) {
            std::fprintf(stderr,"usage: vtr_legend_snapshot config/vtr.toml output.bmp\n");
            return 2;
        }
        auto catalog=reg::vtr::readCatalog(argv[1]);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
            return 2;
        }
        constexpr int w=1100,h=1210;
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
        for (int frame = 0; frame < 4; ++frame) {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui::NewFrame();
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
                    reg::vtr::drawLegend(catalog,error,argv[1]);
                    ImGui::EndTabItem();
                } else {
                    legendVisible = false;
                }
                if (ImGui::BeginTabItem("События")) ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
            ImGui::End();
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
        if (!legendVisible) {
            std::fprintf(stderr,"VTR tab was never visible; rejecting blank screenshot\n");
            return 3;
        }
        // Count pixels differing from the normal ImGui background in the
        // interior, ignoring borders. A blank shot must fail CI, not pass.
        const auto* pixels=static_cast<const std::uint8_t*>(surface->pixels);
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
        if (varied<1500) {
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
