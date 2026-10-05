#pragma once

#include <cstddef>

struct ImTextureData;

namespace reg::remote {

// Advances the lifetime of textures received from NetImgui clients.
// Must run on the thread that owns the Dear ImGui/Vulkan renderer context.
void processNetImguiServerTextures();

// Releases CPU-side NetImgui texture objects. The renderer must release any
// associated Vulkan resources before calling this during final shutdown.
void destroyNetImguiServerTextures();

std::size_t netImguiServerTextureCount() noexcept;
ImTextureData* netImguiServerTexture(std::size_t index) noexcept;

} // namespace reg::remote
