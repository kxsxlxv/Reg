#pragma once
#include "vtr/VtrCatalog.hpp"
#include <filesystem>
#include <string>

namespace reg::vtr {
// Draws a standalone, scrollable preview; no live Jetson data is required.
// The preview uses the embedded reference photo, not incoming video.
void drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path, bool selectPrimitives = false);
} // namespace reg::vtr
