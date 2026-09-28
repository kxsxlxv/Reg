#pragma once

#include "video/VideoFrame.hpp"
#include "vulkan/VulkanVideoFrameAccess.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct AVBufferRef;

namespace reg::render {
class VideoOverlayRecorder;
}

namespace reg::vulkan {

class Swapchain;
class VulkanContext;

class VideoRenderer final {
public:
    explicit VideoRenderer(const VulkanContext& vulkan);
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    // Non-blocking raw-display render. Returns false when the GPU/swapchain is
    // temporarily not ready; the caller should simply try again with the newest frame.
    bool render(
        const video::VideoFramePtr& frame,
        Swapchain& swapchain,
        render::VideoOverlayRecorder* overlay = nullptr);

    // Called on the render thread before an FFmpeg decoder session is
    // destroyed/reopened. Retires GPU work, releases AVFrame references and
    // destroys image views that point into the old hardware-frame pool.
    void resetVideoSession();

    // Queues a one-shot capture of the next frame presented by this renderer.
    // This is an explicit diagnostic readback and is never used by the normal
    // decoded-video presentation path.
    bool requestScreenshot(
        std::filesystem::path path);

    bool screenshotPending() const noexcept {
        return pendingScreenshot_.has_value();
    }

private:
    struct VideoFormat {
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkSamplerYcbcrModelConversion model{VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709};
        VkSamplerYcbcrRange range{VK_SAMPLER_YCBCR_RANGE_ITU_NARROW};
        VkChromaLocation xChromaOffset{VK_CHROMA_LOCATION_COSITED_EVEN};
        VkChromaLocation yChromaOffset{VK_CHROMA_LOCATION_MIDPOINT};
        VkFilter filter{VK_FILTER_NEAREST};

        auto operator<=>(const VideoFormat&) const = default;
    };

    struct SurfaceEntry {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    };

    struct FrameSlot {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        VkSemaphore imageAvailable{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        video::VideoFramePtr retainedFrame;
    };

    struct ScreenshotBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize byteSize{};
        bool coherent{false};
    };

    static constexpr std::size_t kFramesInFlight = 2;
    static constexpr std::uint32_t kMaxCachedVideoSurfaces = 128;

    void createCommandResources();
    void destroyCommandResources();

    VideoFormat describeVideoFormat(const video::VideoFrame& frame) const;
    void ensureVideoResources(const video::VideoFrame& frame);
    void createVideoResources(const video::VideoFrame& frame, const VideoFormat& format);
    void destroyVideoResources();

    void ensureGraphicsPipeline(VkFormat swapchainFormat);
    void destroyGraphicsPipeline();

    SurfaceEntry& surfaceFor(const video::VideoFrame& frame);
    VkShaderModule loadShaderModule(const char* filename) const;

    void syncSwapchainState(const Swapchain& swapchain);
    void destroySwapchainSyncResources();
    void recordSwapchainToColorBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image,
        VkImageLayout oldLayout) const;
    void recordSwapchainToPresentBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image) const;

    void recordSwapchainToTransferBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image) const;

    void recordSwapchainTransferToPresentBarrier(
        VkCommandBuffer commandBuffer,
        VkImage image) const;

    ScreenshotBuffer createScreenshotBuffer(
        VkExtent2D extent) const;

    void destroyScreenshotBuffer(
        ScreenshotBuffer& buffer) const noexcept;

    std::vector<std::uint8_t> readScreenshotBuffer(
        const ScreenshotBuffer& buffer) const;

    VkViewport videoViewport(const video::VideoFrame& frame, VkExtent2D extent) const;

    const VulkanContext& vulkan_;
    VulkanVideoFrameAccess frameAccess_;

    VkCommandPool commandPool_{VK_NULL_HANDLE};
    std::array<FrameSlot, kFramesInFlight> frameSlots_{};
    std::size_t nextFrameSlot_{0};

    VideoFormat videoFormat_{};
    bool videoFormatInitialized_{false};
    AVBufferRef* retainedFramesContext_{nullptr};

    VkSamplerYcbcrConversion ycbcrConversion_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::vector<SurfaceEntry> surfaces_;

    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkFormat pipelineSwapchainFormat_{VK_FORMAT_UNDEFINED};

    VkSwapchainKHR observedSwapchain_{VK_NULL_HANDLE};
    std::vector<VkImageLayout> swapchainImageLayouts_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;

    std::optional<std::filesystem::path>
        pendingScreenshot_;
};

} // namespace reg::vulkan
