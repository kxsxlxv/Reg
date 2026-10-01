#pragma once

#include <cstddef>

namespace reg::remote {

// Advances the lifetime of textures received from NetImgui clients.
// Must run on the thread that owns the Dear ImGui/Vulkan renderer context.
void processNetImguiServerTextures();

// Releases CPU-side NetImgui texture objects. The Vulkan backend must already
// have processed any pending ImTextureStatus_WantDestroy requests before this
// is called during final shutdown.
void destroyNetImguiServerTextures();

std::size_t netImguiServerTextureCount() noexcept;

} // namespace reg::remote
