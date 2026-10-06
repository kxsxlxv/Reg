#include "vulkan/VideoRenderer.hpp"

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE

#include "render/RemoteImGuiRenderer.hpp"
#include "vulkan/VulkanError.hpp"

#include <iostream>
#include <memory>

namespace reg::vulkan {
namespace {

std::weak_ptr<const int> gRawNetImguiOwner;

} // namespace

render::VideoOverlayRecorder*
VideoRenderer::ensureNetImguiOverlay(Swapchain& swapchain) {
    if (!netImguiRoleToken_) {
        if (!gRawNetImguiOwner.expired()) {
            return nullptr;
        }

        netImguiRoleToken_ = std::make_shared<const int>(0);
        gRawNetImguiOwner = netImguiRoleToken_;
    }

    if (netImguiStartupFailed_) {
        return nullptr;
    }

    try {
        if (!remoteOverlay_) {
            auto remote =
                std::make_unique<render::RemoteImGuiRenderer>(
                    vulkan_,
                    swapchain,
                    remote::NetImguiHostConfig{
                        .port = 8888,
                        .maxClients = 1,
                        .activeFps = 60.0F,
                        .inactiveFps = 10.0F,
                        .compression = true,
                    });

            std::cout
                << "[netimgui] Raw overlay server starting tcp=8888\n";
            remoteOverlay_ = std::move(remote);
        }

        auto* remote =
            static_cast<render::RemoteImGuiRenderer*>(
                remoteOverlay_.get());
        remote->prepare(swapchain, true);
        return remoteOverlay_.get();
    } catch (const DeviceLostError&) {
        throw;
    } catch (const std::exception& error) {
        std::cerr
            << "[netimgui] Raw overlay disabled: "
            << error.what()
            << '\n';
        remoteOverlay_.reset();
        netImguiStartupFailed_ = true;
        return nullptr;
    }
}

} // namespace reg::vulkan

#endif
