#include "vulkan/VideoRenderer.hpp"
#include "vulkan/NetImguiSettings.hpp"

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE

#include "render/RemoteImGuiRenderer.hpp"
#include "vulkan/VulkanError.hpp"

#include <exception>
#include <iostream>
#include <memory>

namespace reg::vulkan {
namespace {

std::weak_ptr<const int> gRawNetImguiOwner;
std::uint16_t gNetImguiPort = 8888;
bool gNetImguiEnabled = true;

remote::NetImguiHostConfig rawNetImguiConfig() noexcept {
    return remote::NetImguiHostConfig{
        .port = gNetImguiPort,
        .maxClients = 1,
        .activeFps = 60.0F,
        .inactiveFps = 10.0F,
        .compression = true,
    };
}

std::unique_ptr<remote::NetImguiHost> prestartNetImguiListener() noexcept {
    try {
        auto host = std::make_unique<remote::NetImguiHost>(
            rawNetImguiConfig());
        std::cout
            << "[netimgui] listener prestarted tcp=" << gNetImguiPort << '\n';
        return host;
    } catch (const std::exception& error) {
        std::cerr
            << "[netimgui] listener prestart failed: "
            << error.what()
            << '\n';
    } catch (...) {
        std::cerr
            << "[netimgui] listener prestart failed: unknown error\n";
    }
    return nullptr;
}

// The listener is started explicitly after command-line parsing so a user
// supplied TCP port can take effect before Vulkan and RTSP startup. The
// listener is transferred to the renderer at its first use.
std::unique_ptr<remote::NetImguiHost> gPrestartedNetImguiHost;

} // namespace

void configureNetImguiServer(std::uint16_t port, bool enabled) {
    gNetImguiPort = port;
    gNetImguiEnabled = enabled;
    gPrestartedNetImguiHost.reset();
    if (enabled) {
        gPrestartedNetImguiHost = prestartNetImguiListener();
    }
}

render::VideoOverlayRecorder*
VideoRenderer::ensureNetImguiOverlay(Swapchain& swapchain) {
    // Only the Raw presenter owns the remote UI. The CV window must never
    // opportunistically claim the global TCP listener during a reconnect.
    if (!allowNetImgui_ || !gNetImguiEnabled) return nullptr;
    if (!netImguiRoleToken_) {
        if (!gRawNetImguiOwner.expired()) {
            return nullptr;
        }

        netImguiRoleToken_ = std::make_shared<const int>(0);
        gRawNetImguiOwner = netImguiRoleToken_;
    }

    if (netImguiStartupFailed_) {
        return nullptr;
    }

    try {
        if (!remoteOverlay_) {
            // The prestarted host keeps the selected TCP port live
            // before video rendering begins. RemoteImGuiRenderer owns the real
            // host lifetime because it must stop network producers before GPU
            // texture teardown.
            gPrestartedNetImguiHost.reset();

            auto remote =
                std::make_unique<render::RemoteImGuiRenderer>(
                    vulkan_,
                    swapchain,
                    rawNetImguiConfig());

            std::cout
                << "[netimgui] Raw overlay server attached tcp=" << gNetImguiPort << '\n';
            remoteOverlay_ = std::move(remote);
        }

        auto* remote =
            static_cast<render::RemoteImGuiRenderer*>(
                remoteOverlay_.get());
        remote->prepare(swapchain, true);
        return remoteOverlay_.get();
    } catch (const DeviceLostError&) {
        throw;
    } catch (const std::exception& error) {
        std::cerr
            << "[netimgui] Raw overlay disabled: "
            << error.what()
            << '\n';
        remoteOverlay_.reset();
        netImguiStartupFailed_ = true;
        return nullptr;
    }
}

} // namespace reg::vulkan

#endif
