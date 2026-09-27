#include "vulkan/VulkanContext.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace reg::vulkan {
namespace {

constexpr std::uint32_t kNvidiaVendorId = 0x10DE;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult=" + std::to_string(result));
    }
}

bool containsExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

} // namespace

VulkanContext::VulkanContext(SDL_Window* window, bool requestValidation) {
    createInstance(requestValidation);
    createSurface(window);
    selectPhysicalDevice();
    createDevice();
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanContext::createInstance(bool requestValidation) {
    std::uint32_t loaderApiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        checkVk(vkEnumerateInstanceVersion(&loaderApiVersion), "vkEnumerateInstanceVersion");
    }
    if (loaderApiVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan 1.3 or newer is required");
    }

    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr) {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }

    enabledInstanceExtensions_.reserve(sdlExtensionCount);
    for (Uint32 i = 0; i < sdlExtensionCount; ++i) {
        enabledInstanceExtensions_.emplace_back(sdlExtensions[i]);
    }

    if (requestValidation && hasInstanceLayer(kValidationLayer)) {
        enabledLayers_.push_back(kValidationLayer);
    } else if (requestValidation) {
        std::cerr << "[vulkan] validation requested, but " << kValidationLayer << " is unavailable\n";
    }

    std::vector<const char*> extensionPtrs;
    extensionPtrs.reserve(enabledInstanceExtensions_.size());
    for (const auto& extension : enabledInstanceExtensions_) {
        extensionPtrs.push_back(extension.c_str());
    }

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = "Reg RTSP Vulkan Probe";
    applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.pEngineName = "Reg";
    applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensionPtrs.size());
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
    createInfo.enabledLayerCount = static_cast<std::uint32_t>(enabledLayers_.size());
    createInfo.ppEnabledLayerNames = enabledLayers_.data();

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

void VulkanContext::createSurface(SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
}

void VulkanContext::selectPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices(list)");

    const std::vector<const char*> requiredExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    };

    for (const VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);

        if (properties.vendorID != kNvidiaVendorId || properties.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        if (!deviceSupportsExtensions(device, requiredExtensions)) {
            continue;
        }

        const auto families = queryQueueFamilies(device);
        const auto graphicsIt = std::ranges::find_if(families, [](const QueueFamilyCandidate& family) {
            return (family.flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        });
        const auto presentIt = std::ranges::find_if(families, [](const QueueFamilyCandidate& family) {
            return family.present;
        });

        if (graphicsIt == families.end() || presentIt == families.end()) {
            continue;
        }

        auto videoIt = std::ranges::find_if(families, [graphicsIt](const QueueFamilyCandidate& family) {
            return family.index != graphicsIt->index &&
                   (family.flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0 &&
                   (family.videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0;
        });

        if (videoIt == families.end()) {
            videoIt = std::ranges::find_if(families, [](const QueueFamilyCandidate& family) {
                return (family.flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0 &&
                       (family.videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0;
            });
        }

        if (videoIt == families.end()) {
            continue;
        }

        physicalDevice_ = device;
        deviceProperties_ = properties;

        graphicsQueue_.familyIndex = graphicsIt->index;
        graphicsQueue_.familyQueueCount = graphicsIt->queueCount;
        graphicsQueue_.flags = graphicsIt->flags;
        graphicsQueue_.videoCodecOperations = graphicsIt->videoCodecOperations;

        presentQueue_.familyIndex = presentIt->index;
        presentQueue_.familyQueueCount = presentIt->queueCount;
        presentQueue_.flags = presentIt->flags;
        presentQueue_.videoCodecOperations = presentIt->videoCodecOperations;

        videoDecodeQueue_.familyIndex = videoIt->index;
        videoDecodeQueue_.familyQueueCount = videoIt->queueCount;
        videoDecodeQueue_.flags = videoIt->flags;
        videoDecodeQueue_.videoCodecOperations = videoIt->videoCodecOperations;
        break;
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "No NVIDIA Vulkan 1.3 device with graphics/present and H.264 Vulkan Video decode support was found");
    }

    std::cout << "[vulkan] GPU: " << deviceProperties_.deviceName << "\n";
    std::cout << "[vulkan] graphics queue family: " << graphicsQueue_.familyIndex << "\n";
    std::cout << "[vulkan] present queue family: " << presentQueue_.familyIndex << "\n";
    std::cout << "[vulkan] video decode queue family: " << videoDecodeQueue_.familyIndex << "\n";
    if (videoDecodeQueue_.familyIndex == graphicsQueue_.familyIndex) {
        std::cerr << "[vulkan] warning: video decode and graphics share one queue family; "
                     "a dedicated video queue family is preferred for cross-thread submission\n";
    }
}

void VulkanContext::createDevice() {
    enabledDeviceExtensions_ = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    };

#ifdef VK_KHR_internally_synchronized_queues
    const bool internalQueueSyncExtension = deviceSupportsExtensions(
        physicalDevice_,
        std::vector<const char*>{VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME});
    if (internalQueueSyncExtension) {
        enabledDeviceExtensions_.emplace_back(
            VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME);
    }

    VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR availableInternalQueueSync{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR};
#endif

    VkPhysicalDeviceVulkan11Features available11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features available12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features available13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 availableFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    availableFeatures.pNext = &available11;
    available11.pNext = &available12;
    available12.pNext = &available13;
#ifdef VK_KHR_internally_synchronized_queues
    available13.pNext = internalQueueSyncExtension ? &availableInternalQueueSync : nullptr;
#endif
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &availableFeatures);

    if (available11.samplerYcbcrConversion != VK_TRUE) {
        throw std::runtime_error("The selected GPU does not expose samplerYcbcrConversion");
    }
    if (available12.timelineSemaphore != VK_TRUE) {
        throw std::runtime_error("The selected GPU does not expose timelineSemaphore");
    }
    if (available13.synchronization2 != VK_TRUE) {
        throw std::runtime_error("The selected GPU does not expose synchronization2");
    }
    if (available13.dynamicRendering != VK_TRUE) {
        throw std::runtime_error("The selected GPU does not expose dynamicRendering");
    }

    enabledFeatures_.features = {};
    enabledFeatures_.pNext = &enabledVulkan11Features_;
    enabledVulkan11Features_.samplerYcbcrConversion = VK_TRUE;
    enabledVulkan11Features_.pNext = &enabledVulkan12Features_;
    enabledVulkan12Features_.timelineSemaphore = VK_TRUE;
    enabledVulkan12Features_.pNext = &enabledVulkan13Features_;
    enabledVulkan13Features_.synchronization2 = VK_TRUE;
    enabledVulkan13Features_.dynamicRendering = VK_TRUE;

#ifdef VK_KHR_internally_synchronized_queues
    if (internalQueueSyncExtension &&
        availableInternalQueueSync.internallySynchronizedQueues == VK_TRUE) {
        enabledVulkan13Features_.pNext = &enabledInternalQueueSyncFeatures_;
        enabledInternalQueueSyncFeatures_.internallySynchronizedQueues = VK_TRUE;
        queueCreateFlags_ = VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
        std::cout << "[vulkan] internally synchronized queues enabled\n";
    }
#endif

    if (queueCreateFlags_ == 0 &&
        videoDecodeQueue_.familyIndex == graphicsQueue_.familyIndex) {
        throw std::runtime_error(
            "Video decode and graphics share a Vulkan queue family, but "
            "VK_KHR_internally_synchronized_queues is unavailable. "
            "Phase A requires either a dedicated video-decode queue family or "
            "internally synchronized queues to avoid cross-thread queue submission races.");
    }

    std::set<std::uint32_t> uniqueFamilies{
        graphicsQueue_.familyIndex,
        presentQueue_.familyIndex,
        videoDecodeQueue_.familyIndex,
    };

    constexpr float queuePriority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());
    for (const auto familyIndex : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.flags = queueCreateFlags_;
        queueInfo.queueFamilyIndex = familyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    std::vector<const char*> extensionPtrs;
    extensionPtrs.reserve(enabledDeviceExtensions_.size());
    for (const auto& extension : enabledDeviceExtensions_) {
        extensionPtrs.push_back(extension.c_str());
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &enabledFeatures_;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensionPtrs.size());
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
    createInfo.pEnabledFeatures = nullptr;

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");

    const auto getQueue = [this](std::uint32_t familyIndex, VkQueue* queue) {
        if (queueCreateFlags_ == 0) {
            vkGetDeviceQueue(device_, familyIndex, 0, queue);
            return;
        }

        VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        queueInfo.flags = queueCreateFlags_;
        queueInfo.queueFamilyIndex = familyIndex;
        queueInfo.queueIndex = 0;
        vkGetDeviceQueue2(device_, &queueInfo, queue);
    };

    getQueue(graphicsQueue_.familyIndex, &graphicsQueue_.handle);
    getQueue(presentQueue_.familyIndex, &presentQueue_.handle);
    getQueue(videoDecodeQueue_.familyIndex, &videoDecodeQueue_.handle);
}

bool VulkanContext::hasInstanceLayer(const char* layerName) const {
    std::uint32_t count = 0;
    checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties(count)");
    std::vector<VkLayerProperties> layers(count);
    checkVk(vkEnumerateInstanceLayerProperties(&count, layers.data()), "vkEnumerateInstanceLayerProperties(list)");

    return std::ranges::any_of(layers, [layerName](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, layerName) == 0;
    });
}

bool VulkanContext::hasInstanceExtension(const char* extensionName) const {
    std::uint32_t count = 0;
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
            "vkEnumerateInstanceExtensionProperties(count)");
    std::vector<VkExtensionProperties> extensions(count);
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
            "vkEnumerateInstanceExtensionProperties(list)");
    return containsExtension(extensions, extensionName);
}

bool VulkanContext::deviceSupportsExtensions(
    VkPhysicalDevice device,
    const std::vector<const char*>& extensions) const {
    std::uint32_t count = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> available(count);
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()),
            "vkEnumerateDeviceExtensionProperties(list)");

    return std::ranges::all_of(extensions, [&available](const char* required) {
        return containsExtension(available, required);
    });
}

std::vector<VulkanContext::QueueFamilyCandidate> VulkanContext::queryQueueFamilies(VkPhysicalDevice device) const {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, nullptr);

    std::vector<VkQueueFamilyProperties2> properties(count);
    std::vector<VkQueueFamilyVideoPropertiesKHR> videoProperties(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        properties[i] = VkQueueFamilyProperties2{VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
        videoProperties[i] = VkQueueFamilyVideoPropertiesKHR{VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR};
        properties[i].pNext = &videoProperties[i];
    }

    vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, properties.data());

    std::vector<QueueFamilyCandidate> result;
    result.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        VkBool32 presentSupported = VK_FALSE;
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupported),
                "vkGetPhysicalDeviceSurfaceSupportKHR");

        result.push_back(QueueFamilyCandidate{
            .index = i,
            .queueCount = properties[i].queueFamilyProperties.queueCount,
            .flags = properties[i].queueFamilyProperties.queueFlags,
            .videoCodecOperations = videoProperties[i].videoCodecOperations,
            .present = presentSupported == VK_TRUE,
        });
    }

    return result;
}

} // namespace reg::vulkan
