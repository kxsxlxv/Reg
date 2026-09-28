#include "vulkan/VulkanError.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

void deviceLostHasDedicatedType() {
    bool caught = false;

    try {
        reg::vulkan::checkVk(
            VK_ERROR_DEVICE_LOST,
            "test-device");
    } catch (
        const reg::vulkan::DeviceLostError& error) {
        caught = true;
        require(
            error.result() ==
                VK_ERROR_DEVICE_LOST,
            "device-lost result code mismatch");
    }

    require(
        caught,
        "VK_ERROR_DEVICE_LOST was not classified");
}

void surfaceLostHasDedicatedType() {
    bool caught = false;

    try {
        reg::vulkan::checkSurfaceVk(
            VK_ERROR_SURFACE_LOST_KHR,
            "test-surface");
    } catch (
        const reg::vulkan::SurfaceLostError& error) {
        caught = true;
        require(
            error.result() ==
                VK_ERROR_SURFACE_LOST_KHR,
            "surface-lost result code mismatch");
    }

    require(
        caught,
        "VK_ERROR_SURFACE_LOST_KHR was not classified");
}

void genericVulkanErrorPreservesResult() {
    bool caught = false;

    try {
        reg::vulkan::checkVk(
            VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "test-generic");
    } catch (
        const reg::vulkan::DeviceLostError&) {
        throw std::runtime_error(
            "generic error misclassified as device loss");
    } catch (
        const reg::vulkan::VulkanError& error) {
        caught = true;
        require(
            error.result() ==
                VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "generic Vulkan result code mismatch");
    }

    require(
        caught,
        "generic Vulkan error was not thrown");
}

} // namespace

int main() {
    try {
        deviceLostHasDedicatedType();
        surfaceLostHasDedicatedType();
        genericVulkanErrorPreservesResult();

        std::cout
            << "vulkan_error_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "vulkan_error_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
