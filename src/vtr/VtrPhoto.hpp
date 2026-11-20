#pragma once
#include <imgui.h>

namespace reg::vtr {
// User-provided 1536x804 UAV photograph, downsampled to a compact 320x168
// colour-indexed atlas tile. Image is not neural-generated. Geometry overlays
// are drawn separately using native ImGui draw-list primitives.
void installPreviewPhoto(ImFontAtlas* atlas);
struct PreviewPhoto {
    ImTextureRef texture;
    ImVec2 uv0{};
    ImVec2 uv1{};
    int width{320};
    int height{168};
};
bool previewPhoto(PreviewPhoto& photo);
} // namespace reg::vtr
