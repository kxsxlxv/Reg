#pragma once

#include "remote/NetImguiHost.hpp"
#include "render/VideoOverlayRecorder.hpp"

#include <cstdint>
#include <memory>

namespace reg::vulkan {
class Swapchain;
class VulkanContext;
}

namespace reg::render {

struct NetImguiRuntimeStatus {
    bool active{};
    std::uint16_t port{8888};
    remote::NetImguiHostStatus host{};
};

// Last published runtime state for the Telemetry diagnostics tab. The snapshot
// is lock-free and remains valid even while no remote renderer instance exists.
NetImguiRuntimeStatus netImguiRuntimeStatus() noexcept;

// Vulkan overlay endpoint for Dear ImGui draw data produced by a remote
// NetImgui client. UI construction remains entirely in the remote application.
// prepare() receives/reconstructs the latest remote frame and record() appends
// it to an existing Vulkan dynamic-rendering pass (for example, over Raw video).
class RemoteImGuiRenderer final : public VideoOverlayRecorder {
public:
    RemoteImGuiRenderer(
        const vulkan::VulkanContext& vulkan,
        const vulkan::Swapchain& initialSwapchain,
        remote::NetImguiHostConfig hostConfig = {});
    ~RemoteImGuiRenderer();

    RemoteImGuiRenderer(const RemoteImGuiRenderer&) = delete;
    RemoteImGuiRenderer& operator=(const RemoteImGuiRenderer&) = delete;

    // Main/render-thread update. Non-blocking with respect to Vulkan
    // presentation; NetImgui network exchange runs on its worker threads.
    void prepare(
        const vulkan::Swapchain& swapchain,
        bool active = true);

    // VideoOverlayRecorder implementation. No-op until prepare() has produced
    // correctly-sized remote ImDrawData for the target framebuffer.
    void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) override;

    // Standalone presentation paths retained for diagnostics/smoke tests. Reg's
    // product runtime uses prepare()+record() over the Raw video swapchain.
    bool render(vulkan::Swapchain& swapchain, bool active = true);
    bool renderOrClear(vulkan::Swapchain& swapchain, bool active = true);

    remote::NetImguiHostStatus status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
