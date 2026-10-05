#pragma once

#include "render/VideoOverlayRecorder.hpp"

namespace reg::render {

// Records two overlays into the same active Vulkan rendering pass, preserving
// caller-defined z-order. Null entries are ignored.
class OverlayChainRecorder final : public VideoOverlayRecorder {
public:
    OverlayChainRecorder(
        VideoOverlayRecorder* first,
        VideoOverlayRecorder* second) noexcept
        : first_(first), second_(second) {}

    void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) override {
        if (first_ != nullptr) {
            first_->record(
                commandBuffer,
                colorAttachmentFormat,
                framebufferExtent);
        }
        if (second_ != nullptr) {
            second_->record(
                commandBuffer,
                colorAttachmentFormat,
                framebufferExtent);
        }
    }

private:
    VideoOverlayRecorder* first_{};
    VideoOverlayRecorder* second_{};
};

} // namespace reg::render
