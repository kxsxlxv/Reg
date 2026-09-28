#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace reg::vulkan {

class VulkanError : public std::runtime_error {
public:
    VulkanError(
        VkResult result,
        std::string message)
        : std::runtime_error(std::move(message)),
          result_(result) {}

    VkResult result() const noexcept {
        return result_;
    }

private:
    VkResult result_{VK_SUCCESS};
};

class DeviceLostError final : public VulkanError {
public:
    explicit DeviceLostError(
        std::string operation)
        : VulkanError(
              VK_ERROR_DEVICE_LOST,
              std::move(operation) +
                  " failed: Vulkan device lost") {}
};

class SurfaceLostError final : public VulkanError {
public:
    explicit SurfaceLostError(
        std::string operation)
        : VulkanError(
              VK_ERROR_SURFACE_LOST_KHR,
              std::move(operation) +
                  " failed: Vulkan surface lost") {}
};

inline void checkVk(
    VkResult result,
    const char* operation) {
    if (result == VK_SUCCESS) {
        return;
    }

    if (result == VK_ERROR_DEVICE_LOST) {
        throw DeviceLostError(operation);
    }

    throw VulkanError(
        result,
        std::string(operation) +
            " failed with VkResult=" +
            std::to_string(result));
}

inline void checkSurfaceVk(
    VkResult result,
    const char* operation) {
    if (result ==
        VK_ERROR_SURFACE_LOST_KHR) {
        throw SurfaceLostError(operation);
    }

    checkVk(result, operation);
}

} // namespace reg::vulkan
