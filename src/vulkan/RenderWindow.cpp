#include "vulkan/RenderWindow.hpp"

#include "vulkan/VulkanContext.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdexcept>
#include <string>

namespace reg::vulkan {

RenderWindow::RenderWindow(
    const VulkanContext& vulkan,
    SDL_Window* window,
    PresentPolicy presentPolicy)
    : vulkan_(vulkan),
      window_(window) {
    if (window_ == nullptr) {
        throw std::invalid_argument("RenderWindow requires a valid SDL window");
    }

    if (!SDL_Vulkan_CreateSurface(
            window_,
            vulkan_.instance(),
            nullptr,
            &surface_)) {
        throw std::runtime_error(
            std::string("SDL_Vulkan_CreateSurface failed: ") +
            SDL_GetError());
    }

    try {
        VkBool32 presentSupported = VK_FALSE;
        const VkResult supportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
            vulkan_.physicalDevice(),
            vulkan_.presentQueue().familyIndex,
            surface_,
            &presentSupported);

        if (supportResult != VK_SUCCESS) {
            throw std::runtime_error(
                "vkGetPhysicalDeviceSurfaceSupportKHR failed for RenderWindow");
        }
        if (presentSupported != VK_TRUE) {
            throw std::runtime_error(
                "Selected Vulkan present queue family cannot present to this SDL window");
        }

        swapchain_ = std::make_unique<Swapchain>(
            vulkan_,
            window_,
            surface_,
            presentPolicy);
    } catch (...) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            surface_,
            nullptr);
        surface_ = VK_NULL_HANDLE;
        throw;
    }
}

RenderWindow::~RenderWindow() {
    if (vulkan_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vulkan_.device());
    }

    swapchain_.reset();

    if (surface_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            surface_,
            nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

} // namespace reg::vulkan
