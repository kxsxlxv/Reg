#pragma once
#include "vtr/VtrCatalog.hpp"
#include <filesystem>
#include <string>
#include <imgui.h>

namespace reg::vtr {
// Draws a standalone, scrollable preview; no live Jetson data is required.
// The preview uses the embedded reference photo, not incoming video.
// Shared production and raster-regression entry point.
void drawBbox(ImDrawList* draw,const Box& style,ImVec2 min,ImVec2 max);
enum class LegendPage { None, Outputs, Primitives, Editor };
LegendPage drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path, bool selectPrimitives = false, bool selectEditor = false);
} // namespace reg::vtr
