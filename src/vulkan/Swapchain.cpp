#include "vulkan/Swapchain.hpp"

#include "vulkan/VulkanContext.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace reg::vulkan {
namespace {

void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult=" + std::to_string(result));
    }
}

} // namespace

Swapchain::Swapchain(const VulkanContext& vulkan, SDL_Window* window)
    : vulkan_(vulkan),
      window_(window) {
    if (window_ == nullptr) {
        throw std::invalid_argument("Swapchain requires a valid SDL window");
    }
    if (!recreate()) {
        throw std::runtime_error("Cannot create swapchain for a zero-sized window");
    }
}

Swapchain::~Swapchain() {
    destroySwapchain();
}

bool Swapchain::recreate() {
    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight)) {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }
    if (pixelWidth <= 0 || pixelHeight <= 0) {
        return false;
    }

    checkVk(vkDeviceWaitIdle(vulkan_.device()), "vkDeviceWaitIdle(swapchain recreate)");

    VkSurfaceCapabilitiesKHR capabilities{};
    checkVk(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            vulkan_.physicalDevice(),
            vulkan_.bootstrapSurface(),
            &capabilities),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    std::uint32_t formatCount = 0;
    checkVk(
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            vulkan_.physicalDevice(),
            vulkan_.bootstrapSurface(),
            &formatCount,
            nullptr),
        "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    if (formatCount == 0) {
        throw std::runtime_error("Vulkan surface exposes no formats");
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    checkVk(
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            vulkan_.physicalDevice(),
            vulkan_.bootstrapSurface(),
            &formatCount,
            formats.data()),
        "vkGetPhysicalDeviceSurfaceFormatsKHR(list)");

    std::uint32_t presentModeCount = 0;
    checkVk(
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            vulkan_.physicalDevice(),
            vulkan_.bootstrapSurface(),
            &presentModeCount,
            nullptr),
        "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount != 0) {
        checkVk(
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                vulkan_.physicalDevice(),
                vulkan_.bootstrapSurface(),
                &presentModeCount,
                presentModes.data()),
            "vkGetPhysicalDeviceSurfacePresentModesKHR(list)");
    }

    const auto newSurfaceFormat = chooseSurfaceFormat(formats);
    const auto newPresentMode = choosePresentMode(presentModes);
    const auto newExtent = chooseExtent(capabilities);
    if (newExtent.width == 0 || newExtent.height == 0) {
        return false;
    }

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    const std::uint32_t queueFamilies[]{
        vulkan_.graphicsQueue().familyIndex,
        vulkan_.presentQueue().familyIndex,
    };

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = vulkan_.bootstrapSurface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = newSurfaceFormat.format;
    createInfo.imageColorSpace = newSurfaceFormat.colorSpace;
    createInfo.imageExtent = newExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = newPresentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain_;

    if (queueFamilies[0] != queueFamilies[1]) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilies;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    checkVk(
        vkCreateSwapchainKHR(vulkan_.device(), &createInfo, nullptr, &newSwapchain),
        "vkCreateSwapchainKHR");

    const VkSwapchainKHR oldSwapchain = swapchain_;
    auto oldViews = std::move(imageViews_);

    swapchain_ = newSwapchain;
    surfaceFormat_ = newSurfaceFormat;
    presentMode_ = newPresentMode;
    extent_ = newExtent;

    std::uint32_t actualImageCount = 0;
    checkVk(
        vkGetSwapchainImagesKHR(vulkan_.device(), swapchain_, &actualImageCount, nullptr),
        "vkGetSwapchainImagesKHR(count)");
    images_.resize(actualImageCount);
    checkVk(
        vkGetSwapchainImagesKHR(vulkan_.device(), swapchain_, &actualImageCount, images_.data()),
        "vkGetSwapchainImagesKHR(list)");

    imageViews_.reserve(images_.size());
    for (const VkImage image : images_) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat_.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(vulkan_.device(), &viewInfo, nullptr, &view), "vkCreateImageView(swapchain)");
        imageViews_.push_back(view);
    }

    for (const VkImageView view : oldViews) {
        vkDestroyImageView(vulkan_.device(), view, nullptr);
    }
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vulkan_.device(), oldSwapchain, nullptr);
    }

    return true;
}

AcquireStatus Swapchain::acquire(VkSemaphore imageAvailable, std::uint32_t& imageIndex) {
    const VkResult result = vkAcquireNextImageKHR(
        vulkan_.device(),
        swapchain_,
        0,
        imageAvailable,
        VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL still acquires an image and signals imageAvailable. Consume it
        // normally; vkQueuePresentKHR will request recreation if it remains necessary.
        return AcquireStatus::Ready;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return AcquireStatus::Recreate;
    }
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return AcquireStatus::NotReady;
    }

    checkVk(result, "vkAcquireNextImageKHR");
    return AcquireStatus::NotReady;
}

bool Swapchain::present(std::uint32_t imageIndex, VkSemaphore renderFinished) {
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult result = vkQueuePresentKHR(vulkan_.presentQueue().handle, &presentInfo);
    if (result == VK_SUCCESS) {
        return true;
    }
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }

    checkVk(result, "vkQueuePresentKHR");
    return false;
}

void Swapchain::destroySwapchain() {
    if (vulkan_.device() == VK_NULL_HANDLE) {
        return;
    }
    for (const VkImageView view : imageViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(vulkan_.device(), view, nullptr);
        }
    }
    imageViews_.clear();
    images_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vulkan_.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    const auto preferred = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_SRGB &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (preferred != formats.end()) {
        return *preferred;
    }

    const auto rgba = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_R8G8B8A8_SRGB &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return rgba != formats.end() ? *rgba : formats.front();
}

VkPresentModeKHR Swapchain::choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    // Raw-display policy: minimum latency first. Tearing is explicitly acceptable.
    if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    return VkExtent2D{
        .width = std::clamp(
            static_cast<std::uint32_t>(std::max(width, 0)),
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width),
        .height = std::clamp(
            static_cast<std::uint32_t>(std::max(height, 0)),
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height),
    };
}

} // namespace reg::vulkan
