#include "vulkan/RenderWindow.hpp"

#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

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
      window_(window),
      presentPolicy_(presentPolicy) {
    if (window_ == nullptr) {
        throw std::invalid_argument(
            "RenderWindow requires a valid SDL window");
    }

    if (!createSurfaceAndSwapchain()) {
        throw std::runtime_error(
            "Cannot create initial Vulkan surface/swapchain for window");
    }
}

RenderWindow::~RenderWindow() {
    if (vulkan_.device() != VK_NULL_HANDLE) {
        static_cast<void>(
            vkDeviceWaitIdle(vulkan_.device()));
    }

    destroySurfaceAndSwapchain();
}

Swapchain& RenderWindow::swapchain() {
    if (!swapchain_) {
        throw std::logic_error(
            "RenderWindow swapchain is temporarily unavailable");
    }
    return *swapchain_;
}

const Swapchain& RenderWindow::swapchain() const {
    if (!swapchain_) {
        throw std::logic_error(
            "RenderWindow swapchain is temporarily unavailable");
    }
    return *swapchain_;
}

bool RenderWindow::recoverSurface() {
    checkVk(
        vkDeviceWaitIdle(vulkan_.device()),
        "vkDeviceWaitIdle(surface recovery)");

    destroySurfaceAndSwapchain();

    return createSurfaceAndSwapchain();
}

bool RenderWindow::createSurfaceAndSwapchain() {
    int pixelWidth = 0;
    int pixelHeight = 0;

    if (!SDL_GetWindowSizeInPixels(
            window_,
            &pixelWidth,
            &pixelHeight)) {
        throw std::runtime_error(
            std::string(
                "SDL_GetWindowSizeInPixels failed during surface recovery: ") +
            SDL_GetError());
    }

    if (pixelWidth <= 0 ||
        pixelHeight <= 0) {
        return false;
    }

    VkSurfaceKHR newSurface =
        VK_NULL_HANDLE;

    if (!SDL_Vulkan_CreateSurface(
            window_,
            vulkan_.instance(),
            nullptr,
            &newSurface)) {
        return false;
    }

    try {
        VkBool32 presentSupported =
            VK_FALSE;

        const VkResult supportResult =
            vkGetPhysicalDeviceSurfaceSupportKHR(
                vulkan_.physicalDevice(),
                vulkan_.presentQueue().familyIndex,
                newSurface,
                &presentSupported);

        if (supportResult ==
            VK_ERROR_SURFACE_LOST_KHR) {
            SDL_Vulkan_DestroySurface(
                vulkan_.instance(),
                newSurface,
                nullptr);
            return false;
        }

        checkVk(
            supportResult,
            "vkGetPhysicalDeviceSurfaceSupportKHR(surface recovery)");

        if (presentSupported != VK_TRUE) {
            SDL_Vulkan_DestroySurface(
                vulkan_.instance(),
                newSurface,
                nullptr);
            return false;
        }

        auto newSwapchain =
            std::make_unique<Swapchain>(
                vulkan_,
                window_,
                newSurface,
                presentPolicy_);

        surface_ = newSurface;
        swapchain_ =
            std::move(newSwapchain);
        return true;
    } catch (const SurfaceLostError&) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            newSurface,
            nullptr);
        return false;
    } catch (...) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            newSurface,
            nullptr);
        throw;
    }
}

void RenderWindow::destroySurfaceAndSwapchain() noexcept {
    swapchain_.reset();

    if (surface_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(
            vulkan_.instance(),
            surface_,
            nullptr);
        surface_ =
            VK_NULL_HANDLE;
    }
}

} // namespace reg::vulkan
