#pragma once

#include "video/VideoFrame.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanVideoFrameAccess.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

struct AVBufferRef;

namespace reg::render {
class VideoOverlayRecorder;
}

namespace reg::vulkan {

class BlankRenderer;
class VulkanContext;

class VideoRenderer final {
public:
    explicit VideoRenderer(const VulkanContext& vulkan);
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    // Raw path. With NetImgui enabled this overload claims the first video
    // output as the Raw presentation endpoint and composites remote ImGui over
    // the decoded frame. Other callers can use the explicit-overlay overload.
    bool render(
        const video::VideoFramePtr& frame,
        Swapchain& swapchain) {
#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
        return render(frame, swapchain, ensureNetImguiOverlay(swapchain));
#else
        return render(frame, swapchain, nullptr);
#endif
    }

    // Non-blocking video render with an explicit caller-owned overlay. Returns
    // false when the GPU/swapchain is temporarily not ready.
    bool render(
        const video::VideoFramePtr& frame,
        Swapchain& swapchain,
        render::VideoOverlayRecorder* overlay);

    // Presents a black frame, optionally with an ImGui overlay. Used for
    // explicit NO SIGNAL presentation without stopping/restarting the decoder.
    bool renderBlank(
        Swapchain& swapchain,
        render::VideoOverlayRecorder* overlay = nullptr);

    // Called on the render thread before an FFmpeg decoder session is
    // destroyed/reopened. Presents black to the last video swapchain first so
    // a disconnected source never leaves a frozen stale frame on screen, then
    // retires GPU work and releases old hardware-frame resources.
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
        VkDeviceSize allocationSize{};
        bool coherent{false};
    };

    // Non-owning pointer whose null comparison also verifies that the
    // RenderWindow has not destroyed/replaced the referenced Swapchain object.
    // This preserves the existing lightweight render call while making the
    // reset-time black-frame presentation safe across surface recovery.
    struct TrackedSwapchainPtr {
        Swapchain* pointer{nullptr};
        std::weak_ptr<const int> lifetime;

        TrackedSwapchainPtr& operator=(Swapchain* value) noexcept {
            pointer = value;
            lifetime = value != nullptr
                ? value->lifetimeToken()
                : std::weak_ptr<const int>{};
            return *this;
        }

        bool operator!=(std::nullptr_t) const noexcept {
            return pointer != nullptr && !lifetime.expired();
        }

        Swapchain& operator*() const noexcept {
            return *pointer;
        }
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

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    render::VideoOverlayRecorder* ensureNetImguiOverlay(Swapchain& swapchain);
#endif

    const VulkanContext& vulkan_;
    VulkanVideoFrameAccess frameAccess_;
    std::unique_ptr<BlankRenderer> blankRenderer_;
    TrackedSwapchainPtr lastSwapchain_{};

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    std::unique_ptr<render::VideoOverlayRecorder> remoteOverlay_;
    std::shared_ptr<const int> netImguiRoleToken_;
    bool netImguiStartupFailed_{false};
#endif

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
