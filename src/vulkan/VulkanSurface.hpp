#pragma once

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace reg::vulkan {

class VulkanContext;

class VulkanSurface final {
public:
    VulkanSurface(
        const VulkanContext& vulkan,
        SDL_Window* window);
    ~VulkanSurface();

    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;
    VulkanSurface(VulkanSurface&&) = delete;
    VulkanSurface& operator=(VulkanSurface&&) = delete;

    VkSurfaceKHR handle() const noexcept { return surface_; }
    SDL_Window* window() const noexcept { return window_; }

private:
    const VulkanContext& vulkan_;
    SDL_Window* window_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
};

} // namespace reg::vulkan
