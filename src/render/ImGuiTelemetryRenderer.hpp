#pragma once

#include "telemetry/TelemetryModel.hpp"

#include <memory>

namespace reg::vulkan {
class Swapchain;
class VulkanContext;
}

namespace reg::render {

class ImGuiTelemetryRenderer final {
public:
    ImGuiTelemetryRenderer(
        const vulkan::VulkanContext& vulkan,
        const vulkan::Swapchain& initialSwapchain);
    ~ImGuiTelemetryRenderer();

    ImGuiTelemetryRenderer(
        const ImGuiTelemetryRenderer&) = delete;
    ImGuiTelemetryRenderer& operator=(
        const ImGuiTelemetryRenderer&) = delete;

    // Non-blocking. Returns false when WSI/GPU frame slots are temporarily
    // unavailable. Telemetry frames may be skipped without affecting video.
    bool render(
        const telemetry::Snapshot& snapshot,
        vulkan::Swapchain& swapchain);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render

#include "render/TelemetryUiBridge.hpp"
