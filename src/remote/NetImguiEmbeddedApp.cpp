#include "remote/NetImguiEmbeddedBridge.hpp"

#include "NetImguiServer_App.h"
#include "NetImguiServer_RemoteClient.h"

#include <NetImgui_Api.h>
#include <Private/NetImgui_CmdPackets.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<NetImguiServer::App::ServerTexture*> gServerTextures;

bool textureCanBeginDestroy(
    const NetImguiServer::App::ServerTexture& texture) {
    if (texture.mOwnerClientIndex < 0) {
        return true;
    }

    const auto owner = static_cast<std::uint32_t>(texture.mOwnerClientIndex);
    if (owner >= NetImguiServer::RemoteClient::Client::GetCountMax()) {
        return true;
    }

    const auto& client = NetImguiServer::RemoteClient::Client::Get(owner);
    return client.mpImguiDrawData == nullptr ||
        client.mpImguiDrawData->mFrameIndex > texture.mLastFrameUsed;
}

} // namespace

namespace NetImguiServer::App {

ServerTexture* CreateTexture(
    const NetImgui::Internal::CmdTexture& command,
    std::uint32_t textureDataSize) {
    if (command.mFormat == NetImgui::eTexFormat::kTexFmtCustom ||
        command.mpTextureData.Get() == nullptr) {
        return nullptr;
    }

    auto* texture = new ServerTexture{};
    texture->mClientTexID =
        static_cast<ImTextureID>(command.mTextureClientID);
    texture->mIsUpdatable = command.mUpdatable;

    texture->mTexData.Create(
        ImTextureFormat_RGBA32,
        static_cast<int>(command.mWidth),
        static_cast<int>(command.mHeight));

    if (command.mFormat == NetImgui::eTexFormat::kTexFmtRGBA8) {
        const auto expected = static_cast<std::size_t>(command.mWidth) *
            static_cast<std::size_t>(command.mHeight) * 4U;
        if (textureDataSize < expected) {
            delete texture;
            return nullptr;
        }
        std::memcpy(
            texture->mTexData.Pixels,
            command.mpTextureData.Get(),
            expected);
    } else if (command.mFormat == NetImgui::eTexFormat::kTexFmtA8) {
        const auto pixelCount = static_cast<std::size_t>(command.mWidth) *
            static_cast<std::size_t>(command.mHeight);
        if (textureDataSize < pixelCount) {
            delete texture;
            return nullptr;
        }

        const auto* source = command.mpTextureData.Get();
        auto* destination = reinterpret_cast<std::uint32_t*>(
            texture->mTexData.GetPixels());
        for (std::size_t i = 0; i < pixelCount; ++i) {
            destination[i] = 0x00FFFFFFU |
                (static_cast<std::uint32_t>(source[i]) << 24U);
        }
    } else {
        delete texture;
        return nullptr;
    }

    texture->mTexData.Status = ImTextureStatus_WantCreate;
    texture->mTexData.UseColors =
        command.mFormat == NetImgui::eTexFormat::kTexFmtRGBA8;
    ImGui::RegisterUserTexture(&texture->mTexData);
    gServerTextures.push_back(texture);
    return texture;
}

bool CreateTexture_Custom(
    ServerTexture&,
    const NetImgui::Internal::CmdTexture&,
    std::uint32_t) {
    return false;
}

bool DestroyTexture_Custom(
    ServerTexture&,
    const NetImgui::Internal::CmdTexture&,
    std::uint32_t) {
    return false;
}

bool HAL_Startup(const char*) {
    return true;
}

void HAL_Shutdown() {}

bool HAL_GetSocketInfo(
    NetImgui::Internal::Network::SocketInfo*,
    char* outHostname,
    std::size_t hostnameLength,
    int& outPort) {
    if (outHostname != nullptr && hostnameLength > 0U) {
        outHostname[0] = '\0';
    }
    outPort = 0;
    return false;
}

const char* HAL_GetUserSettingFolder() {
    return nullptr;
}

bool HAL_GetClipboardUpdated() {
    return false;
}

void HAL_RenderDrawData(ImDrawData*) {}
void HAL_RenderDrawData(RemoteClient::Client&, ImDrawData*) {}

bool HAL_CreateRenderTarget(
    std::uint16_t,
    std::uint16_t,
    void*& outRenderTarget,
    ImTextureData&) {
    outRenderTarget = nullptr;
    return false;
}

void HAL_DestroyRenderTarget(
    void*& renderTarget,
    ImTextureData&) {
    renderTarget = nullptr;
}

} // namespace NetImguiServer::App

namespace reg::remote {

void processNetImguiServerTextures() {
    for (auto it = gServerTextures.begin(); it != gServerTextures.end();) {
        auto* texture = *it;
        if (texture == nullptr) {
            it = gServerTextures.erase(it);
            continue;
        }

        auto& data = texture->mTexData;

        if (!texture->mIsUpdatable &&
            data.Pixels != nullptr &&
            data.Status == ImTextureStatus_OK) {
            data.DestroyPixels();
        }

        if (data.Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(&data);
            delete texture;
            it = gServerTextures.erase(it);
            continue;
        }

        if (data.WantDestroyNextFrame && textureCanBeginDestroy(*texture)) {
            if (data.UnusedFrames++ > 0) {
                data.Status = ImTextureStatus_WantDestroy;
            }
        }

        ++it;
    }
}

void destroyNetImguiServerTextures() {
    for (auto* texture : gServerTextures) {
        if (texture == nullptr) {
            continue;
        }
        ImGui::UnregisterUserTexture(&texture->mTexData);
        delete texture;
    }
    gServerTextures.clear();
}

std::size_t netImguiServerTextureCount() noexcept {
    return gServerTextures.size();
}

} // namespace reg::remote
