#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace reg::vulkan {

struct QueueInfo {
    std::uint32_t familyIndex{VK_QUEUE_FAMILY_IGNORED};
    VkQueue handle{VK_NULL_HANDLE};
    VkQueueFlags flags{};
    VkVideoCodecOperationFlagsKHR videoCodecOperations{};
};

class VulkanContext {
public:
    VulkanContext(SDL_Window* window, bool requestValidation);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    VkDevice device() const noexcept { return device_; }
    VkSurfaceKHR bootstrapSurface() const noexcept { return surface_; }

    const QueueInfo& graphicsQueue() const noexcept { return graphicsQueue_; }
    const QueueInfo& presentQueue() const noexcept { return presentQueue_; }
    const QueueInfo& videoDecodeQueue() const noexcept { return videoDecodeQueue_; }

    const VkPhysicalDeviceFeatures2& enabledFeatures() const noexcept { return enabledFeatures_; }
    const std::vector<std::string>& enabledInstanceExtensions() const noexcept { return enabledInstanceExtensions_; }
    const std::vector<std::string>& enabledDeviceExtensions() const noexcept { return enabledDeviceExtensions_; }

    const VkPhysicalDeviceProperties& physicalDeviceProperties() const noexcept { return deviceProperties_; }

private:
    struct QueueFamilyCandidate {
        std::uint32_t index{};
        VkQueueFlags flags{};
        VkVideoCodecOperationFlagsKHR videoCodecOperations{};
        bool present{};
    };

    void createInstance(bool requestValidation);
    void createSurface(SDL_Window* window);
    void selectPhysicalDevice();
    void createDevice();

    bool hasInstanceLayer(const char* layerName) const;
    bool hasInstanceExtension(const char* extensionName) const;
    bool deviceSupportsExtensions(VkPhysicalDevice device, const std::vector<const char*>& extensions) const;
    std::vector<QueueFamilyCandidate> queryQueueFamilies(VkPhysicalDevice device) const;

    VkInstance instance_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};

    VkPhysicalDeviceProperties deviceProperties_{};

    QueueInfo graphicsQueue_{};
    QueueInfo presentQueue_{};
    QueueInfo videoDecodeQueue_{};

    std::vector<std::string> enabledInstanceExtensions_;
    std::vector<std::string> enabledDeviceExtensions_;
    std::vector<const char*> enabledLayers_;

    VkPhysicalDeviceFeatures2 enabledFeatures_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features enabledVulkan11Features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features enabledVulkan12Features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features enabledVulkan13Features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
};

} // namespace reg::vulkan
