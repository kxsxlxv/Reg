#include "vulkan/RenderWindow.hpp"

#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <unordered_map>
#endif

namespace reg::vulkan {
namespace {

#ifdef _WIN32
std::unordered_map<HWND, WNDPROC> gNoActivateWndProcs;

HWND win32Hwnd(SDL_Window* window) noexcept {
    if (window == nullptr) {
        return nullptr;
    }

    const SDL_PropertiesID properties =
        SDL_GetWindowProperties(window);
    if (properties == 0) {
        return nullptr;
    }

    return static_cast<HWND>(
        SDL_GetPointerProperty(
            properties,
            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
            nullptr));
}

LRESULT CALLBACK noActivateWndProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == WM_MOUSEACTIVATE) {
        std::cerr
            << "[window-activation] blocked WM_MOUSEACTIVATE hwnd="
            << static_cast<void*>(hwnd)
            << '\n';
        return MA_NOACTIVATE;
    }

    const auto it = gNoActivateWndProcs.find(hwnd);
    if (it == gNoActivateWndProcs.end() ||
        it->second == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return CallWindowProcW(
        it->second,
        hwnd,
        message,
        wParam,
        lParam);
}

void enforceWin32NoActivate(SDL_Window* window) {
    if (window == nullptr ||
        (SDL_GetWindowFlags(window) &
         SDL_WINDOW_NOT_FOCUSABLE) == 0U) {
        return;
    }

    HWND hwnd = win32Hwnd(window);
    if (hwnd == nullptr) {
        throw std::runtime_error(
            "SDL did not expose a Win32 HWND for a non-focusable window");
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR currentExStyle =
        GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (currentExStyle == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        throw std::runtime_error(
            "GetWindowLongPtrW(GWL_EXSTYLE) failed: " +
            std::to_string(GetLastError()));
    }

    const LONG_PTR requiredExStyle =
        currentExStyle |
        static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    if (requiredExStyle != currentExStyle) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previousExStyle =
            SetWindowLongPtrW(
                hwnd,
                GWL_EXSTYLE,
                requiredExStyle);
        if (previousExStyle == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            throw std::runtime_error(
                "SetWindowLongPtrW(GWL_EXSTYLE) failed: " +
                std::to_string(GetLastError()));
        }
    }

    if (!gNoActivateWndProcs.contains(hwnd)) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previousWndProc =
            SetWindowLongPtrW(
                hwnd,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(
                    &noActivateWndProc));
        if (previousWndProc == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            throw std::runtime_error(
                "SetWindowLongPtrW(GWLP_WNDPROC) failed: " +
                std::to_string(GetLastError()));
        }

        gNoActivateWndProcs.emplace(
            hwnd,
            reinterpret_cast<WNDPROC>(previousWndProc));
    }

    if (!SetWindowPos(
            hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED |
                SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOZORDER |
                SWP_NOOWNERZORDER |
                SWP_NOACTIVATE)) {
        throw std::runtime_error(
            "SetWindowPos(SWP_NOACTIVATE) failed: " +
            std::to_string(GetLastError()));
    }
}

void restoreWin32WndProc(SDL_Window* window) noexcept {
    HWND hwnd = win32Hwnd(window);
    if (hwnd == nullptr) {
        return;
    }

    const auto it = gNoActivateWndProcs.find(hwnd);
    if (it == gNoActivateWndProcs.end()) {
        return;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR result =
        SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(it->second));
    if (result == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        std::cerr
            << "[window-activation] failed to restore WNDPROC error="
            << GetLastError()
            << '\n';
    }

    gNoActivateWndProcs.erase(it);
}
#else
void enforceWin32NoActivate(SDL_Window*) {}
void restoreWin32WndProc(SDL_Window*) noexcept {}
#endif

} // namespace

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

    enforceWin32NoActivate(window_);

    try {
        if (!createSurfaceAndSwapchain()) {
            throw std::runtime_error(
                "Cannot create initial Vulkan surface/swapchain for window");
        }
    } catch (...) {
        restoreWin32WndProc(window_);
        throw;
    }
}

RenderWindow::~RenderWindow() {
    if (vulkan_.device() != VK_NULL_HANDLE) {
        static_cast<void>(
            vkDeviceWaitIdle(vulkan_.device()));
    }

    destroySurfaceAndSwapchain();
    restoreWin32WndProc(window_);
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
    enforceWin32NoActivate(window_);

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
