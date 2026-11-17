#pragma once

#include "media/GtMapWorker.hpp"
#include "vulkan/VulkanContext.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace reg::vulkan {

// The GT scalar maps are auxiliary sampled images, not CPU video readbacks.
// Neither the compositor nor its command buffers retain an AVFrame/AVVkFrame.
struct GtMapStyle {
    bool sky{false};
    bool depth{false};
    bool skySmooth{false};
    bool depthSmooth{true};
    float skyOpacity{0.35F};
    float depthOpacity{0.70F};
    float nearM{1.0F};
    float farM{250.0F};
};

class GtMapCompositor final {
public:
    explicit GtMapCompositor(const VulkanContext& vulkan);
    ~GtMapCompositor();
    GtMapCompositor(const GtMapCompositor&)=delete;
    GtMapCompositor& operator=(const GtMapCompositor&)=delete;

    // Called only when VideoRenderer's matching frame-slot fence is signalled.
    // Records host->device copies before vkCmdBeginRendering. Returns false
    // when neither independently validated scalar map is present.
    bool prepare(
        VkCommandBuffer commandBuffer,
        std::size_t slotIndex,
        VkFormat colorFormat,
        const std::shared_ptr<const media::GtDecodedBundle>& bundle,
        const GtMapStyle& style);

    // Draw after RGB and before bbox/ImGui in the same dynamic rendering pass.
    void draw(VkCommandBuffer commandBuffer, std::size_t slotIndex,
              const GtMapStyle& style) const;

private:
    struct Texture {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory imageMemory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkBuffer staging{VK_NULL_HANDLE};
        VkDeviceMemory stagingMemory{VK_NULL_HANDLE};
        void* mapped{};
        VkExtent2D extent{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        std::uint64_t epoch{}, frameId{}, sourceTime{};
        bool cached{};
    };
    struct Slot {
        Texture sky, depth;
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        bool skyReady{}, depthReady{}, validityPlane{};
        std::uint32_t skyWidth{}, skyHeight{}, depthWidth{}, depthHeight{};
    };
    static constexpr std::size_t kSlots=2;
    static constexpr std::uint32_t kSkyByteRow=1024;
    std::uint32_t findMemoryType(std::uint32_t bits,
                                 VkMemoryPropertyFlags flags) const;
    void ensureTexture(Texture& texture, VkFormat format, VkExtent2D extent);
    void destroyTexture(Texture& texture) noexcept;
    void updateDescriptor(Slot& slot) const;
    void copyTexture(VkCommandBuffer commandBuffer, Texture& texture,
                     const std::uint8_t* bytes, std::size_t length);
    void ensurePipeline(VkFormat format);
    void destroyPipeline() noexcept;

    const VulkanContext& vulkan_;
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::array<Slot,kSlots> slots_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkFormat colorFormat_{VK_FORMAT_UNDEFINED};
};
} // namespace reg::vulkan
