#pragma once

#include "remote/NetImguiHost.hpp"

#include <memory>

namespace reg::vulkan {
class Swapchain;
class VulkanContext;
}

namespace reg::render {

// Vulkan presentation endpoint for Dear ImGui draw data produced by a remote
// NetImgui client. UI construction remains entirely in the remote application.
class RemoteImGuiRenderer final {
public:
    RemoteImGuiRenderer(
        const vulkan::VulkanContext& vulkan,
        const vulkan::Swapchain& initialSwapchain,
        remote::NetImguiHostConfig hostConfig = {});
    ~RemoteImGuiRenderer();

    RemoteImGuiRenderer(const RemoteImGuiRenderer&) = delete;
    RemoteImGuiRenderer& operator=(const RemoteImGuiRenderer&) = delete;

    // Non-blocking with respect to Vulkan presentation. The NetImgui network
    // exchange itself runs on worker threads owned by NetImgui.
    bool render(vulkan::Swapchain& swapchain, bool active = true);

    remote::NetImguiHostStatus status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
