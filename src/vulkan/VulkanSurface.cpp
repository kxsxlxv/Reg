#include "vulkan/VulkanSurface.hpp"

#include "vulkan/VulkanContext.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdexcept>
#include <string>

namespace reg::vulkan {

VulkanSurface::VulkanSurface(
    const VulkanContext& vulkan,
    SDL_Window* window)
    : vulkan_(vulkan),
      window_(window) {
    if (window_ == nullptr) {
        throw std::invalid_argument("VulkanSurface requires a valid SDL window");
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

    VkBool32 presentSupported = VK_FALSE;
    const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(
        vulkan_.physicalDevice(),
        vulkan_.presentQueue().familyIndex,
        surface_,
        &presentSupported);

    if (result != VK_SUCCESS) {
        SDL_Vulkan_DestroySurface(vulkan_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        throw std::runtime_error(
            "vkGetPhysicalDeviceSurfaceSupportKHR failed for additional surface");
    }

    if (presentSupported != VK_TRUE) {
        SDL_Vulkan_DestroySurface(vulkan_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        throw std::runtime_error(
            "The selected Vulkan present queue cannot present to the additional window surface");
    }
}

VulkanSurface::~VulkanSurface() {
    if (surface_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            surface_,
            nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

} // namespace reg::vulkan
