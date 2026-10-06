#include "vulkan/VideoRenderer.hpp"

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE

#include "render/RemoteImGuiRenderer.hpp"
#include "vulkan/VulkanError.hpp"

#include <exception>
#include <iostream>
#include <memory>

namespace reg::vulkan {
namespace {

std::weak_ptr<const int> gRawNetImguiOwner;

remote::NetImguiHostConfig rawNetImguiConfig() noexcept {
    return remote::NetImguiHostConfig{
        .port = 8888,
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
            << "[netimgui] build enabled; listener prestarted tcp=8888\n";
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

// Start the embedded receiving side as soon as the NetImgui-enabled module is
// loaded. This keeps TCP :8888 independent of RTSP availability and of the
// first Raw render/NO-SIGNAL pass. Ownership is transferred to the Vulkan
// renderer on first use by stopping this lightweight listener and immediately
// recreating the host inside RemoteImGuiRenderer.
std::unique_ptr<remote::NetImguiHost> gPrestartedNetImguiHost =
    prestartNetImguiListener();

} // namespace

render::VideoOverlayRecorder*
VideoRenderer::ensureNetImguiOverlay(Swapchain& swapchain) {
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
            // The prestarted host exists only to guarantee that :8888 is live
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
                << "[netimgui] Raw overlay server attached tcp=8888\n";
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
