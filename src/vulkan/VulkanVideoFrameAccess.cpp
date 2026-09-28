#include "vulkan/VulkanVideoFrameAccess.hpp"

#include "video/VideoFrame.hpp"
#include "vulkan/VulkanContext.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <stdexcept>

namespace reg::vulkan {

VulkanVideoFrameAccess::LockedFrame VulkanVideoFrameAccess::lock(
    const video::VideoFrame& frame) const {
    AVFrame* avFrame = frame.avFrame();
    AVVkFrame* vkFrame = frame.vkFrame();

    if (avFrame == nullptr || vkFrame == nullptr || avFrame->hw_frames_ctx == nullptr) {
        throw std::runtime_error("Vulkan video frame is missing FFmpeg hardware-frame context");
    }
    if (vkFrame->img[0] == VK_NULL_HANDLE || vkFrame->sem[0] == VK_NULL_HANDLE) {
        throw std::runtime_error("AVVkFrame has no primary image or timeline semaphore");
    }

    // Phase A.2 supports FFmpeg's native multiplanar image representation.
    // Separate VkImage-per-plane output is rejected rather than copied or converted.
    if (vkFrame->img[1] != VK_NULL_HANDLE) {
        throw std::runtime_error(
            "AVVkFrame uses multiple VkImages; Phase A.2 requires a single multiplanar sampled VkImage");
    }

    auto* framesContext =
        reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
    if (framesContext == nullptr || framesContext->hwctx == nullptr) {
        throw std::runtime_error("AVHWFramesContext is unavailable for Vulkan frame");
    }

    auto* vulkanFramesContext =
        reinterpret_cast<AVVulkanFramesContext*>(framesContext->hwctx);
    if (vulkanFramesContext->lock_frame == nullptr ||
        vulkanFramesContext->unlock_frame == nullptr) {
        throw std::runtime_error("FFmpeg Vulkan frame locking callbacks are unavailable");
    }

    vulkanFramesContext->lock_frame(framesContext, vkFrame);

    LockedFrame locked{
        .framesContext = framesContext,
        .vulkanFramesContext = vulkanFramesContext,
        .vulkanFrame = vkFrame,
        .semaphore = vkFrame->sem[0],
        .waitValue = vkFrame->sem_value[0],
        .signalValue = vkFrame->sem_value[0] + 1,
        .locked = true,
    };

    const std::uint32_t queueFamily = vkFrame->queue_family[0];
    if (queueFamily != VK_QUEUE_FAMILY_IGNORED &&
        queueFamily != vulkan_.graphicsQueue().familyIndex) {
        unlock(locked);
        throw std::runtime_error(
            "Decoded VkImage is exclusively owned by a non-graphics queue family. "
            "The shared FFmpeg Vulkan device must expose both decode and graphics queue families "
            "so decode surfaces are allocated concurrently.");
    }

    return locked;
}

void VulkanVideoFrameAccess::recordSampleBarrier(
    VkCommandBuffer commandBuffer,
    LockedFrame& frame) const {
    if (!frame.locked || frame.vulkanFrame == nullptr) {
        throw std::logic_error("recordSampleBarrier requires a locked AVVkFrame");
    }

    AVVkFrame* vkFrame = frame.vulkanFrame;

    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Decode completion and memory availability are carried by the AVVkFrame
    // timeline semaphore wait in vkQueueSubmit2. The previous access happened
    // on FFmpeg's queue, so this receiving-queue layout transition must not
    // claim that foreign access in its local source scope.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = vkFrame->layout[0];
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vkFrame->img[0];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkSemaphoreSubmitInfo VulkanVideoFrameAccess::waitInfo(const LockedFrame& frame) const noexcept {
    VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    info.semaphore = frame.semaphore;
    info.value = frame.waitValue;
    info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    info.deviceIndex = 0;
    return info;
}

VkSemaphoreSubmitInfo VulkanVideoFrameAccess::signalInfo(const LockedFrame& frame) const noexcept {
    VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    info.semaphore = frame.semaphore;
    info.value = frame.signalValue;
    info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    info.deviceIndex = 0;
    return info;
}

void VulkanVideoFrameAccess::commitSubmission(LockedFrame& frame) const {
    if (!frame.locked || frame.vulkanFrame == nullptr) {
        throw std::logic_error("commitSubmission requires a locked AVVkFrame");
    }

    frame.vulkanFrame->layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    frame.vulkanFrame->access[0] = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    frame.vulkanFrame->sem_value[0] = frame.signalValue;
    frame.submitted = true;
}

void VulkanVideoFrameAccess::unlock(LockedFrame& frame) const noexcept {
    if (!frame.locked) {
        return;
    }

    frame.vulkanFramesContext->unlock_frame(frame.framesContext, frame.vulkanFrame);
    frame.locked = false;
}

} // namespace reg::vulkan
