#pragma once

#include "render/OverlayPrimitives.hpp"
#include "render/VideoOverlayRecorder.hpp"

#include <memory>

namespace reg::vulkan {
class Swapchain;
class VulkanContext;
}

namespace reg::render {

class ImGuiOverlayRenderer final : public VideoOverlayRecorder {
public:
    ImGuiOverlayRenderer(
        const vulkan::VulkanContext& vulkan,
        const vulkan::Swapchain& initialSwapchain);
    ~ImGuiOverlayRenderer() override;

    ImGuiOverlayRenderer(const ImGuiOverlayRenderer&) = delete;
    ImGuiOverlayRenderer& operator=(const ImGuiOverlayRenderer&) = delete;

    void prepare(
        const OverlayScene& scene,
        const vulkan::Swapchain& swapchain);

    void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
