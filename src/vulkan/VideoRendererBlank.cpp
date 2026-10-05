#include "vulkan/VideoRenderer.hpp"

#include "vulkan/BlankRenderer.hpp"

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
#include "render/OverlayChainRecorder.hpp"
#endif

namespace reg::vulkan {

bool VideoRenderer::renderBlank(
    Swapchain& swapchain,
    render::VideoOverlayRecorder* overlay) {
    lastSwapchain_ = &swapchain;
    if (!blankRenderer_) {
        return false;
    }

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    render::RemoteImGuiRenderer* remoteOverlay =
        ensureNetImguiOverlay(swapchain);

    if (remoteOverlay != nullptr) {
        if (overlay == nullptr) {
            return blankRenderer_->render(
                swapchain,
                remoteOverlay);
        }

        render::OverlayChainRecorder chain(
            overlay,
            remoteOverlay);
        return blankRenderer_->render(
            swapchain,
            &chain);
    }
#endif

    return blankRenderer_->render(swapchain, overlay);
}

} // namespace reg::vulkan
