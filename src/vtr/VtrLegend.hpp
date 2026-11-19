#pragma once
#include "vtr/VtrCatalog.hpp"
#include <filesystem>
#include <string>

namespace reg::vtr {
// Draws a standalone, scrollable preview; no live Jetson data is required.
// The preview uses an artificial UAV silhouette, not the incoming video.
void drawLegend(Catalog& catalog, std::string& error,
                const std::filesystem::path& path);
} // namespace reg::vtr
