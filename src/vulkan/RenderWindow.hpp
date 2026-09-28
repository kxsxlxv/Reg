#pragma once

#include "vulkan/Swapchain.hpp"

#include <vulkan/vulkan.h>

#include <memory>

struct SDL_Window;

namespace reg::vulkan {

class VulkanContext;

class RenderWindow final {
public:
    RenderWindow(
        const VulkanContext& vulkan,
        SDL_Window* window,
        PresentPolicy presentPolicy);
    ~RenderWindow();

    RenderWindow(const RenderWindow&) = delete;
    RenderWindow& operator=(const RenderWindow&) = delete;

    SDL_Window* sdlWindow() const noexcept { return window_; }
    VkSurfaceKHR surface() const noexcept { return surface_; }

    Swapchain& swapchain() noexcept { return *swapchain_; }
    const Swapchain& swapchain() const noexcept { return *swapchain_; }

private:
    const VulkanContext& vulkan_;
    SDL_Window* window_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> swapchain_;
};

} // namespace reg::vulkan
