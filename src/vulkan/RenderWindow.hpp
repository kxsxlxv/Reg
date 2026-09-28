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

    bool available() const noexcept {
        return swapchain_ != nullptr;
    }

    Swapchain& swapchain();
    const Swapchain& swapchain() const;

    // Rebuilds VkSurfaceKHR + VkSwapchainKHR for the existing SDL_Window.
    // Returns false for a temporarily unavailable/zero-sized window; Vulkan
    // device-loss and other non-WSI errors still propagate.
    bool recoverSurface();

private:
    bool createSurfaceAndSwapchain();
    void destroySurfaceAndSwapchain() noexcept;

    const VulkanContext& vulkan_;
    SDL_Window* window_{nullptr};
    PresentPolicy presentPolicy_{PresentPolicy::Stable};

    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> swapchain_;
};

} // namespace reg::vulkan
