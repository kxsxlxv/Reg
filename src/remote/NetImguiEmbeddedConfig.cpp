#include <NetImgui_Api.h>
#include <Private/NetImgui_Shared.h>

#include "NetImguiServer_Config.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace NetImguiServer::Config {
namespace {

std::mutex gConfigMutex;
std::vector<Client> gConfigs;
Client::RuntimeID gNextRuntimeId = 1;

std::vector<Client>::iterator findConfig(Client::RuntimeID id) {
    return std::find_if(
        gConfigs.begin(),
        gConfigs.end(),
        [id](const Client& config) {
            return config.mRuntimeID == id;
        });
}

std::vector<Client>::const_iterator findConfigConst(Client::RuntimeID id) {
    return std::find_if(
        gConfigs.cbegin(),
        gConfigs.cend(),
        [id](const Client& config) {
            return config.mRuntimeID == id;
        });
}

} // namespace

std::uint32_t Server::sPort = NetImgui::kDefaultServerPort;
float Server::sRefreshFPSActive = 30.0F;
float Server::sRefreshFPSInactive = 10.0F;
float Server::sDPIScaleRatio = 1.0F;
bool Server::sCompressionEnable = true;
float Server::sFontSize = 16.0F;
int Server::sWindowPlacement[4] = {100, 100, 1280, 720};
bool Server::sWindowMaximized = false;

Client::Client()
    : mHostPort(NetImgui::kDefaultClientPort),
      mRuntimeID(kInvalidRuntimeID),
      mConfigType(eConfigType::Transient),
      mDPIScaleEnabled(true),
      mBlockTakeover(false),
      mReadOnly(true),
      mConnectAuto(false),
      mConnectRequest(false),
      mConnectForce(false),
      mConnectStatus(eStatus::Disconnected),
      mConnectLastTime(std::chrono::steady_clock::now()) {
    NetImgui::Internal::StringCopy(mClientName, "Embedded client");
    NetImgui::Internal::StringCopy(mHostName, "localhost");
}

void Client::SetConfig(const Client& config) {
    std::scoped_lock lock(gConfigMutex);
    auto copy = config;
    if (copy.mRuntimeID == kInvalidRuntimeID) {
        copy.mRuntimeID = gNextRuntimeId++;
    }

    const auto found = findConfig(copy.mRuntimeID);
    if (found == gConfigs.end()) {
        gConfigs.push_back(copy);
    } else {
        *found = copy;
    }
}

void Client::DelConfig(std::uint32_t configID) {
    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfig(configID);
    if (found != gConfigs.end()) {
        gConfigs.erase(found);
    }
}

bool Client::GetConfigByID(std::uint32_t configID, Client& outConfig) {
    if (configID == kInvalidRuntimeID) {
        return false;
    }

    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfigConst(configID);
    if (found == gConfigs.cend()) {
        return false;
    }
    outConfig = *found;
    return true;
}

bool Client::GetConfigByIndex(std::uint32_t index, Client& outConfig) {
    std::scoped_lock lock(gConfigMutex);
    if (index >= gConfigs.size()) {
        return false;
    }
    outConfig = gConfigs[index];
    return true;
}

std::uint32_t Client::GetConfigCount() {
    std::scoped_lock lock(gConfigMutex);
    return static_cast<std::uint32_t>(gConfigs.size());
}

bool Client::GetProperty_BlockTakeover(std::uint32_t configID) {
    if (configID == kInvalidRuntimeID) {
        return false;
    }

    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfigConst(configID);
    return found != gConfigs.cend() && found->mBlockTakeover;
}

void Client::SetProperty_Status(std::uint32_t configID, eStatus status) {
    if (configID == kInvalidRuntimeID) {
        return;
    }

    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfig(configID);
    if (found != gConfigs.end()) {
        found->mConnectStatus = status;
    }
}

void Client::SetProperty_ConnectAuto(std::uint32_t configID, bool value) {
    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfig(configID);
    if (found != gConfigs.end()) {
        found->mConnectAuto = value;
    }
}

void Client::SetProperty_ConnectRequest(
    std::uint32_t configID,
    bool value,
    bool force) {
    std::scoped_lock lock(gConfigMutex);
    const auto found = findConfig(configID);
    if (found != gConfigs.end()) {
        found->mConnectRequest = value && !force;
        found->mConnectForce = value && force;
        found->mConnectLastTime = std::chrono::steady_clock::now();
    }
}

void Client::SaveAll() {
    // Reg owns its configuration. Upstream NetImgui JSON persistence is
    // intentionally excluded from the embedded server core.
}

void Client::LoadAll() {
    // Reg starts with no outbound NetImgui client configurations. The intended
    // topology is that the headless simulator connects to Reg's listening port.
}

void Client::Clear() {
    std::scoped_lock lock(gConfigMutex);
    gConfigs.clear();
}

bool Client::ShouldSave(eConfigType) const {
    return false;
}

void Client::SaveConfigFile(eConfigType, bool) {}
void Client::LoadConfigFile(eConfigType) {}

} // namespace NetImguiServer::Config
