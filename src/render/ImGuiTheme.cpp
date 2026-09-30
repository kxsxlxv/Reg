#include "render/ImGuiTheme.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef REG_FONT_DIR
#error "REG_FONT_DIR must point to the configured Reg font asset directory"
#endif

namespace reg::render {
namespace {

std::filesystem::path fontPath(const char* filename) {
    return std::filesystem::path(REG_FONT_DIR) / filename;
}

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    const auto robotoPath = fontPath("Roboto.ttf");
    const auto symbolsPath = fontPath("MaterialSymbolsOutlined.ttf");

    if (!std::filesystem::exists(robotoPath)) {
        throw std::runtime_error(
            std::string("Roboto font asset is missing: ") +
            robotoPath.string());
    }
    if (!std::filesystem::exists(symbolsPath)) {
        throw std::runtime_error(
            std::string("Material Symbols font asset is missing: ") +
            symbolsPath.string());
    }

    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;

    ImFont* roboto = io.Fonts->AddFontFromFileTTF(
        robotoPath.string().c_str(),
        18.0F,
        &fontConfig,
        io.Fonts->GetGlyphRangesCyrillic());
    if (roboto == nullptr) {
        throw std::runtime_error(
            std::string("Failed to load Roboto font: ") +
            robotoPath.string());
    }
    io.FontDefault = roboto;

    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.OversampleH = 2;
    iconConfig.OversampleV = 2;

    // Keep the Material Symbols atlas deliberately small. The underlying font
    // is the official Google variable font, but Reg currently needs only these
    // status glyphs. Add new explicit codepoints here as the UI grows.
    static constexpr ImWchar iconRanges[]{
        0xE002, 0xE002, // warning
        0xE04C, 0xE04C, // videocam_off
        0xF0BE, 0xF0BE, // check_circle
        0,
    };

    if (io.Fonts->AddFontFromFileTTF(
            symbolsPath.string().c_str(),
            18.0F,
            &iconConfig,
            iconRanges) == nullptr) {
        throw std::runtime_error(
            std::string("Failed to load Material Symbols font: ") +
            symbolsPath.string());
    }
}

void applyStyle(float contentScale) {
    contentScale = std::max(contentScale, 0.25F);

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(contentScale);
    style.FontScaleDpi = contentScale;

    // Geometry — use float literals, ScaleAllSizes will handle DPI.
    style.WindowRounding = 0.0F;
    style.ChildRounding = 0.0F;
    style.FrameRounding = 0.0F;
    style.PopupRounding = 0.0F;
    style.ScrollbarRounding = 2.0F;
    style.GrabRounding = 2.0F;
    style.TabRounding = 2.0F;

    // Palette — blue accent on dark background.
    ImVec4* c = style.Colors;

    // Core backgrounds.
    c[ImGuiCol_WindowBg] = ImVec4(0.102F, 0.114F, 0.137F, 1.000F); // #1A1D23
    c[ImGuiCol_PopupBg] = ImVec4(0.118F, 0.129F, 0.157F, 0.960F);

    // Borders.
    c[ImGuiCol_BorderShadow] = ImVec4(0.000F, 0.000F, 0.000F, 0.000F);

    // Frame backgrounds.
    c[ImGuiCol_FrameBg] = ImVec4(0.145F, 0.157F, 0.188F, 1.000F); // #252830
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.180F, 0.192F, 0.224F, 1.000F);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.212F, 0.224F, 0.255F, 1.000F);

    // Title bar.
    c[ImGuiCol_TitleBg] = ImVec4(0.082F, 0.094F, 0.125F, 1.000F); // #151820
    c[ImGuiCol_TitleBgActive] = ImVec4(0.102F, 0.114F, 0.137F, 1.000F); // #1A1D23
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.082F, 0.094F, 0.125F, 0.750F);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.102F, 0.114F, 0.137F, 1.000F);

    // Scrollbar.
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.102F, 0.114F, 0.137F, 0.600F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.290F, 0.561F, 0.851F, 0.700F);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.290F, 0.561F, 0.851F, 1.000F);

    // Accent blue: #4A90D9.
    c[ImGuiCol_CheckMark] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);
    c[ImGuiCol_SliderGrab] = ImVec4(0.290F, 0.565F, 0.851F, 0.800F);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.353F, 0.627F, 0.914F, 1.000F); // #5BA0E9

    // Buttons.
    c[ImGuiCol_Button] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.290F, 0.565F, 0.851F, 0.600F);
    c[ImGuiCol_ButtonActive] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);

    // Headers.
    c[ImGuiCol_Header] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.290F, 0.565F, 0.851F, 0.400F);
    c[ImGuiCol_HeaderActive] = ImVec4(0.290F, 0.565F, 0.851F, 0.700F);

    // Separators.
    c[ImGuiCol_Separator] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.290F, 0.565F, 0.851F, 0.700F);
    c[ImGuiCol_SeparatorActive] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);

    // Resize grip.
    c[ImGuiCol_ResizeGrip] = ImVec4(0.208F, 0.220F, 0.251F, 0.500F);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.290F, 0.565F, 0.851F, 0.600F);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0.290F, 0.565F, 0.851F, 0.900F);

    // Tabs.
    c[ImGuiCol_Tab] = ImVec4(0.145F, 0.157F, 0.188F, 1.000F);
    c[ImGuiCol_TabHovered] = ImVec4(0.284F, 0.505F, 0.980F, 1.000F);
    c[ImGuiCol_TabSelected] = ImVec4(0.224F, 0.435F, 0.980F, 1.000F);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);
    c[ImGuiCol_TabDimmed] = ImVec4(0.102F, 0.114F, 0.137F, 1.000F);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.176F, 0.353F, 0.549F, 1.000F);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);

    // Plot colors.
    c[ImGuiCol_PlotLines] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);
    c[ImGuiCol_PlotLinesHovered] = ImVec4(0.353F, 0.627F, 0.914F, 1.000F);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.353F, 0.627F, 0.914F, 1.000F);

    // Tables.
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.145F, 0.157F, 0.188F, 1.000F);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.208F, 0.220F, 0.251F, 1.000F);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.208F, 0.220F, 0.251F, 0.600F);
    c[ImGuiCol_TableRowBg] = ImVec4(0.000F, 0.000F, 0.000F, 0.000F);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.000F, 1.000F, 1.000F, 0.020F);

    // Text.
    c[ImGuiCol_Text] = ImVec4(0.878F, 0.878F, 0.878F, 1.000F); // #E0E0E0
    c[ImGuiCol_TextDisabled] = ImVec4(0.439F, 0.439F, 0.439F, 1.000F); // #707070
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.290F, 0.565F, 0.851F, 0.350F);
    c[ImGuiCol_TextLink] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);

    // Misc.
    c[ImGuiCol_DragDropTarget] = ImVec4(0.290F, 0.565F, 0.851F, 0.900F);
    c[ImGuiCol_NavCursor] = ImVec4(0.290F, 0.565F, 0.851F, 1.000F);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.000F, 0.000F, 0.000F, 0.600F);

    if ((ImGui::GetIO().ConfigFlags) != 0) {
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }
}

} // namespace

void configureRegImGuiTheme(float contentScale) {
    loadFonts();
    applyStyle(contentScale);

    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
}

} // namespace reg::render
