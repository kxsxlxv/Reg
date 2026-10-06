#pragma once

#include "vulkan/VulkanError.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

struct SDL_Window;

namespace reg::vulkan {

class VulkanContext;

enum class AcquireStatus {
    Ready,
    Recreate,
    NotReady,
};

enum class PresentPolicy {
    LowLatencyTearingAllowed,
    Stable,
};

class Swapchain final {
public:
    Swapchain(
        const VulkanContext& vulkan,
        SDL_Window* window,
        VkSurfaceKHR surface,
        PresentPolicy presentPolicy);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool recreate();

    AcquireStatus acquire(VkSemaphore imageAvailable, std::uint32_t& imageIndex);
    bool present(std::uint32_t imageIndex, VkSemaphore renderFinished);

    VkSwapchainKHR handle() const noexcept { return swapchain_; }
    VkFormat format() const noexcept { return surfaceFormat_.format; }
    VkColorSpaceKHR colorSpace() const noexcept { return surfaceFormat_.colorSpace; }
    VkExtent2D extent() const noexcept { return extent_; }
    VkImage image(std::uint32_t index) const noexcept { return images_[index]; }
    VkImageView imageView(std::uint32_t index) const noexcept { return imageViews_[index]; }
    std::size_t imageCount() const noexcept { return images_.size(); }
    VkSurfaceKHR surface() const noexcept { return surface_; }
    SDL_Window* window() const noexcept { return window_; }
    float contentScale() const noexcept;
    bool transferSourceSupported() const noexcept {
        return transferSourceSupported_;
    }

    std::weak_ptr<const int> lifetimeToken() const noexcept {
        return lifetimeToken_;
    }

private:
    void destroySwapchain();
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    const VulkanContext& vulkan_;
    SDL_Window* window_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    PresentPolicy presentPolicy_{PresentPolicy::Stable};

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkSurfaceFormatKHR surfaceFormat_{};
    VkPresentModeKHR presentMode_{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D extent_{};
    bool transferSourceSupported_{false};

    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::shared_ptr<const int> lifetimeToken_{
        std::make_shared<const int>(0)};
};

} // namespace reg::vulkan