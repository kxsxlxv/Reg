#pragma once

#include "telemetry/TelemetryModel.hpp"

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
#include "render/RemoteImGuiRenderer.hpp"
#endif

#include <cstdint>
#include <memory>

namespace reg::vulkan {
class Swapchain;
class VulkanContext;
}

namespace reg::render {

struct NetImguiDiagnostics {
    bool enabled{};
    bool listening{};
    bool connected{};
    std::uint16_t port{8888};
    std::uint32_t connectedClients{};
    std::uint64_t bytesReceived{};
    std::uint64_t bytesSent{};
};

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

    bool render(
        const telemetry::Snapshot& snapshot,
        vulkan::Swapchain& swapchain,
        const NetImguiDiagnostics& netImgui);

    // Non-blocking. Returns false when WSI/GPU frame slots are temporarily
    // unavailable. Telemetry frames may be skipped without affecting video.
    bool render(
        const telemetry::Snapshot& snapshot,
        vulkan::Swapchain& swapchain) {
#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
        const NetImguiRuntimeStatus runtime =
            netImguiRuntimeStatus();
        return render(
            snapshot,
            swapchain,
            NetImguiDiagnostics{
                .enabled = true,
                .listening = runtime.host.listening,
                .connected = runtime.host.connected,
                .port = runtime.port,
                .connectedClients = runtime.host.connectedClients,
                .bytesReceived = runtime.host.bytesReceived,
                .bytesSent = runtime.host.bytesSent,
            });
#else
        return render(snapshot, swapchain, NetImguiDiagnostics{});
#endif
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::render
