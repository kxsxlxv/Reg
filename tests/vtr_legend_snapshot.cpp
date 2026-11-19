// Offline snapshot of the exact ImGui VTR legend renderer used by Reg.
// Uses SDL's software surface renderer, so no RTSP/Jetson/Vulkan/GPU needed.
#include "vtr/VtrCatalog.hpp"
#include "vtr/VtrLegend.hpp"
#include "render/ImGuiTheme.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
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
        constexpr int w=1100,h=2180;
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
        if(!ImGui_ImplSDLRenderer3_Init(renderer)) {
            std::fprintf(stderr,"ImGui software backend init failed\n");
            return 2;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0},ImGuiCond_Always);
        ImGui::SetNextWindowSize({static_cast<float>(w),static_cast<float>(h)},ImGuiCond_Always);
        ImGui::Begin("Reg Telemetry",nullptr,
            ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings);
        if(ImGui::BeginTabBar("telemetry_tabs")) {
            if(ImGui::BeginTabItem("Обзор")) ImGui::EndTabItem();
            if(ImGui::BeginTabItem("Exact Sync")) ImGui::EndTabItem();
            if(ImGui::BeginTabItem("NetImgui")) ImGui::EndTabItem();
            if(ImGui::BeginTabItem("VTR / Легенда",nullptr,ImGuiTabItemFlags_SetSelected)) {
                std::string error;
                reg::vtr::drawLegend(catalog,error,argv[1]);
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("События")) ImGui::EndTabItem();
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer,18,22,30,255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        SDL_RenderPresent(renderer);
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
