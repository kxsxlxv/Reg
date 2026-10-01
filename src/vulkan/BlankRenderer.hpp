#pragma once

#include "render/VideoOverlayRecorder.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace reg::vulkan {

class BlankRenderer final {
public:
    explicit BlankRenderer(const VulkanContext& vulkan)
        : vulkan_(vulkan) {
        createCommandResources();
    }

    ~BlankRenderer() {
        if (vulkan_.device() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vulkan_.device());
        }
        destroySwapchainSyncResources();
        for (auto& slot : frameSlots_) {
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(vulkan_.device(), slot.fence, nullptr);
                slot.fence = VK_NULL_HANDLE;
            }
            if (slot.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(vulkan_.device(), slot.imageAvailable, nullptr);
                slot.imageAvailable = VK_NULL_HANDLE;
            }
        }
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vulkan_.device(), commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
    }

    BlankRenderer(const BlankRenderer&) = delete;
    BlankRenderer& operator=(const BlankRenderer&) = delete;

    bool render(
        Swapchain& swapchain,
        render::VideoOverlayRecorder* overlay = nullptr) {
        FrameSlot& slot = frameSlots_[nextFrameSlot_];

        const VkResult fenceStatus =
            vkGetFenceStatus(vulkan_.device(), slot.fence);
        if (fenceStatus == VK_NOT_READY) {
            return false;
        }
        checkVk(fenceStatus, "vkGetFenceStatus(blank)");

        syncSwapchainState(swapchain);

        std::uint32_t imageIndex = 0;
        const AcquireStatus acquireStatus =
            swapchain.acquire(slot.imageAvailable, imageIndex);
        if (acquireStatus == AcquireStatus::NotReady) {
            return false;
        }
        if (acquireStatus == AcquireStatus::Recreate) {
            if (swapchain.recreate()) {
                observedSwapchain_ = VK_NULL_HANDLE;
            }
            return false;
        }

        checkVk(
            vkResetFences(vulkan_.device(), 1, &slot.fence),
            "vkResetFences(blank)");
        checkVk(
            vkResetCommandBuffer(slot.commandBuffer, 0),
            "vkResetCommandBuffer(blank)");

        VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(
            vkBeginCommandBuffer(slot.commandBuffer, &beginInfo),
            "vkBeginCommandBuffer(blank)");

        recordSwapchainToColorBarrier(
            slot.commandBuffer,
            swapchain.image(imageIndex),
            swapchainImageLayouts_.at(imageIndex));

        VkClearValue clear{};
        clear.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderingAttachmentInfo colorAttachment{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = swapchain.imageView(imageIndex);
        colorAttachment.imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clear;

        VkRenderingInfo renderingInfo{
            VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea.extent = swapchain.extent();
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(slot.commandBuffer, &renderingInfo);
        if (overlay != nullptr) {
            overlay->record(
                slot.commandBuffer,
                swapchain.format(),
                swapchain.extent());
        }
        vkCmdEndRendering(slot.commandBuffer);

        recordSwapchainToPresentBarrier(
            slot.commandBuffer,
            swapchain.image(imageIndex));

        checkVk(
            vkEndCommandBuffer(slot.commandBuffer),
            "vkEndCommandBuffer(blank)");

        VkSemaphoreSubmitInfo imageWait{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        imageWait.semaphore = slot.imageAvailable;
        imageWait.stageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        const VkSemaphore renderFinished =
            renderFinishedSemaphores_.at(imageIndex);

        VkSemaphoreSubmitInfo presentSignal{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        presentSignal.semaphore = renderFinished;
        presentSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo commandInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = slot.commandBuffer;

        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &imageWait;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &presentSignal;

        checkVk(
            vkQueueSubmit2(
                vulkan_.graphicsQueue().handle,
                1,
                &submitInfo,
                slot.fence),
            "vkQueueSubmit2(blank)");

        swapchainImageLayouts_.at(imageIndex) =
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        const bool presentOk =
            swapchain.present(imageIndex, renderFinished);

        nextFrameSlot_ =
            (nextFrameSlot_ + 1) % frameSlots_.size();

        if (!presentOk) {
            if (swapchain.recreate()) {
                observedSwapchain_ = VK_NULL_HANDLE;
            }
            return false;
        }

        return true;
    }

private:
    struct FrameSlot {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        VkSemaphore imageAvailable{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
    };

    static constexpr std::size_t kFramesInFlight = 2;

    void createCommandResources() {
        VkCommandPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = vulkan_.graphicsQueue().familyIndex;

        checkVk(
            vkCreateCommandPool(
                vulkan_.device(),
                &poolInfo,
                nullptr,
                &commandPool_),
            "vkCreateCommandPool(blank)");

        std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
        VkCommandBufferAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount =
            static_cast<std::uint32_t>(commandBuffers.size());

        checkVk(
            vkAllocateCommandBuffers(
                vulkan_.device(),
                &allocateInfo,
                commandBuffers.data()),
            "vkAllocateCommandBuffers(blank)");

        for (std::size_t i = 0; i < frameSlots_.size(); ++i) {
            frameSlots_[i].commandBuffer = commandBuffers[i];

            VkSemaphoreCreateInfo semaphoreInfo{
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            checkVk(
                vkCreateSemaphore(
                    vulkan_.device(),
                    &semaphoreInfo,
                    nullptr,
                    &frameSlots_[i].imageAvailable),
                "vkCreateSemaphore(blank imageAvailable)");

            VkFenceCreateInfo fenceInfo{
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            checkVk(
                vkCreateFence(
                    vulkan_.device(),
                    &fenceInfo,
                    nullptr,
                    &frameSlots_[i].fence),
                "vkCreateFence(blank)");
        }
    }

    void syncSwapchainState(const Swapchain& swapchain) {
        if (observedSwapchain_ == swapchain.handle() &&
            swapchainImageLayouts_.size() == swapchain.imageCount() &&
            renderFinishedSemaphores_.size() == swapchain.imageCount()) {
            return;
        }

        destroySwapchainSyncResources();
        observedSwapchain_ = swapchain.handle();
        swapchainImageLayouts_.assign(
            swapchain.imageCount(),
            VK_IMAGE_LAYOUT_UNDEFINED);
        renderFinishedSemaphores_.resize(
            swapchain.imageCount(),
            VK_NULL_HANDLE);

        VkSemaphoreCreateInfo info{
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& semaphore : renderFinishedSemaphores_) {
            checkVk(
                vkCreateSemaphore(
                    vulkan_.device(),
                    &info,
                    nullptr,
                    &semaphore),
                "vkCreateSemaphore(blank renderFinished)");
        }
    }

    void destroySwapchainSyncResources() noexcept {
        for (VkSemaphore semaphore : renderFinishedSemaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(vulkan_.device(), semaphore, nullptr);
            }
        }
        renderFinishedSemaphores_.clear();
        swapchainImageLayouts_.clear();
        observedSwapchain_ = VK_NULL_HANDLE;
    }

    static void recordSwapchainToColorBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image,
        VkImageLayout oldLayout) {
        VkImageMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = oldLayout;
        barrier.newLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    static void recordSwapchainToPresentBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image) {
        VkImageMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = 0;
        barrier.oldLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    const VulkanContext& vulkan_;
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    std::array<FrameSlot, kFramesInFlight> frameSlots_{};
    std::size_t nextFrameSlot_{};

    VkSwapchainKHR observedSwapchain_{VK_NULL_HANDLE};
    std::vector<VkImageLayout> swapchainImageLayouts_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
};

} // namespace reg::vulkan
