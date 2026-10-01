#include "remote/NetImguiHost.hpp"

#include "remote/NetImguiEmbeddedBridge.hpp"

#include "NetImguiServer_Config.h"
#include "NetImguiServer_Network.h"
#include "NetImguiServer_RemoteClient.h"

#include <Private/NetImgui_CmdPackets.h>
#include <Private/NetImgui_Shared.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace reg::remote {
namespace {

std::uint16_t clampDimension(std::uint32_t value) noexcept {
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(
        value,
        std::numeric_limits<std::uint16_t>::max()));
}

void queueFrameRequest(
    NetImguiServer::RemoteClient::Client& client,
    std::uint32_t width,
    std::uint32_t height,
    float desiredFps,
    bool compression) {
    auto* input = client.TakePendingInput();
    if (input == nullptr) {
        input = NetImgui::Internal::netImguiNew<
            NetImgui::Internal::CmdInput>();
    }

    input->mScreenSize[0] = clampDimension(width);
    input->mScreenSize[1] = clampDimension(height);

    // Input forwarding is added in the next integration slice. Until then keep
    // the pointer well outside the drawable area and explicitly clear any state
    // if this packet was reclaimed before the network thread consumed it.
    input->mMousePos[0] = -30000;
    input->mMousePos[1] = -30000;
    input->mMouseWheelVert = 0.0F;
    input->mMouseWheelHoriz = 0.0F;
    input->mMouseDownMask = 0;
    input->mKeyCharCount = 0;
    std::memset(input->mKeyChars, 0, sizeof(input->mKeyChars));
    std::memset(input->mInputDownMask, 0, sizeof(input->mInputDownMask));
    std::memset(input->mInputAnalog, 0, sizeof(input->mInputAnalog));

    input->mCompressionUse = compression;
    input->mCompressionSkip = client.mbCompressionSkipOncePending;
    input->mFontDPIScaling = 1.0F;
    input->mDesiredFps = std::max(desiredFps, 1.0F);

    client.mbCompressionSkipOncePending = false;
    client.mPendingInputOut.Assign(input);
    client.mLastUpdateTime = std::chrono::steady_clock::now();
}

} // namespace

struct NetImguiHost::Impl {
    explicit Impl(NetImguiHostConfig value)
        : config(value) {
        if (config.maxClients == 0U) {
            throw std::invalid_argument(
                "NetImguiHost requires at least one client slot");
        }
        if (config.port == 0U) {
            throw std::invalid_argument(
                "NetImguiHost requires a non-zero TCP port");
        }

        NetImguiServer::Config::Server::sPort = config.port;
        NetImguiServer::Config::Server::sRefreshFPSActive =
            std::max(config.activeFps, 1.0F);
        NetImguiServer::Config::Server::sRefreshFPSInactive =
            std::max(config.inactiveFps, 1.0F);
        NetImguiServer::Config::Server::sCompressionEnable =
            config.compression;

        if (!NetImguiServer::RemoteClient::Client::Startup(
                config.maxClients)) {
            throw std::runtime_error(
                "NetImgui remote-client storage initialization failed");
        }
        clientsStarted = true;

        if (!NetImguiServer::Network::Startup()) {
            NetImguiServer::RemoteClient::Client::Shutdown();
            clientsStarted = false;
            throw std::runtime_error(
                "NetImgui network initialization failed");
        }
        networkStarted = true;
    }

    ~Impl() {
        if (networkStarted) {
            NetImguiServer::Network::Shutdown();
            networkStarted = false;
        }
        if (clientsStarted) {
            NetImguiServer::RemoteClient::Client::Shutdown();
            clientsStarted = false;
        }
    }

    void update(
        std::uint32_t width,
        std::uint32_t height,
        bool active) {
        currentDrawData = nullptr;
        connectedCount = 0U;

        const auto now = std::chrono::steady_clock::now();
        const float desiredFps = active
            ? std::max(config.activeFps, 1.0F)
            : std::max(config.inactiveFps, 1.0F);
        const auto requestPeriod = std::chrono::duration<double>{
            1.0 / static_cast<double>(desiredFps)};
        const bool requestDue = !lastRequestValid ||
            active != previousActive ||
            now - lastRequest >= requestPeriod;

        for (std::uint32_t i = 0;
             i < NetImguiServer::RemoteClient::Client::GetCountMax();
             ++i) {
            auto& client = NetImguiServer::RemoteClient::Client::Get(i);

            if (client.mbIsReleased) {
                client.Uninitialize();
                continue;
            }
            if (!client.mbIsConnected) {
                continue;
            }

            ++connectedCount;
            client.mbIsVisible = true;
            client.mbIsActive = active;

            client.ProcessPendingTextureCmds();

            if (requestDue) {
                queueFrameRequest(
                    client,
                    width,
                    height,
                    desiredFps,
                    config.compression);
            }

            if (currentDrawData == nullptr) {
                currentDrawData = client.GetImguiDrawData(
                    ImTextureID_Invalid);
            }
        }

        if (requestDue && connectedCount > 0U) {
            lastRequest = now;
            lastRequestValid = true;
            previousActive = active;
        }

        processNetImguiServerTextures();
    }

    NetImguiHostConfig config;
    bool clientsStarted{};
    bool networkStarted{};
    bool lastRequestValid{};
    bool previousActive{true};
    std::uint32_t connectedCount{};
    ImDrawData* currentDrawData{};
    std::chrono::steady_clock::time_point lastRequest{};
};

NetImguiHost::NetImguiHost(NetImguiHostConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

NetImguiHost::~NetImguiHost() = default;

void NetImguiHost::update(
    std::uint32_t width,
    std::uint32_t height,
    bool active) {
    impl_->update(width, height, active);
}

ImDrawData* NetImguiHost::drawData() const noexcept {
    return impl_->currentDrawData;
}

NetImguiHostStatus NetImguiHost::status() const noexcept {
    return NetImguiHostStatus{
        .listening = NetImguiServer::Network::IsWaitingForConnection(),
        .connected = impl_->connectedCount > 0U,
        .connectedClients = impl_->connectedCount,
        .bytesReceived = NetImguiServer::Network::GetStatsDataRcvd(),
        .bytesSent = NetImguiServer::Network::GetStatsDataSent(),
    };
}

} // namespace reg::remote
