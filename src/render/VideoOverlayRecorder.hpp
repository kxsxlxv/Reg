#pragma once

#include <vulkan/vulkan.h>

namespace reg::render {

class VideoOverlayRecorder {
public:
    virtual ~VideoOverlayRecorder() = default;

    virtual void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) = 0;
};

} // namespace reg::render
