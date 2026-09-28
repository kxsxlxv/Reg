#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

struct AVHWFramesContext;
struct AVVulkanFramesContext;
struct AVVkFrame;

namespace reg::video {
class VideoFrame;
}

namespace reg::vulkan {

class VulkanContext;

class VulkanVideoFrameAccess final {
public:
    struct LockedFrame {
        AVHWFramesContext* framesContext{nullptr};
        AVVulkanFramesContext* vulkanFramesContext{nullptr};
        AVVkFrame* vulkanFrame{nullptr};

        VkSemaphore semaphore{VK_NULL_HANDLE};
        std::uint64_t waitValue{};
        std::uint64_t signalValue{};

        bool locked{false};
        bool submitted{false};
    };

    explicit VulkanVideoFrameAccess(const VulkanContext& vulkan)
        : vulkan_(vulkan) {}

    LockedFrame lock(const video::VideoFrame& frame) const;
    void recordSampleBarrier(VkCommandBuffer commandBuffer, LockedFrame& frame) const;
    VkSemaphoreSubmitInfo waitInfo(const LockedFrame& frame) const noexcept;
    VkSemaphoreSubmitInfo signalInfo(const LockedFrame& frame) const noexcept;
    void commitSubmission(LockedFrame& frame) const;
    void unlock(LockedFrame& frame) const noexcept;

private:
    const VulkanContext& vulkan_;
};

} // namespace reg::vulkan
