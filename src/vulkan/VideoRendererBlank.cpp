#include "vulkan/VideoRenderer.hpp"

#include "vulkan/BlankRenderer.hpp"

namespace reg::vulkan {

bool VideoRenderer::renderBlank(
    Swapchain& swapchain,
    render::VideoOverlayRecorder* overlay) {
    lastSwapchain_ = &swapchain;
    if (!blankRenderer_) {
        return false;
    }
    return blankRenderer_->render(swapchain, overlay);
}

} // namespace reg::vulkan
