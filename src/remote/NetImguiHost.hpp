#pragma once

#include <cstdint>
#include <memory>

struct ImDrawData;

namespace reg::remote {

struct NetImguiHostConfig {
    std::uint16_t port{8888};
    std::uint32_t maxClients{1};
    float activeFps{60.0F};
    float inactiveFps{10.0F};
    bool compression{true};
};

struct NetImguiHostStatus {
    bool listening{};
    bool connected{};
    std::uint32_t connectedClients{};
    std::uint64_t bytesReceived{};
    std::uint64_t bytesSent{};
};

// Embedded receiving side of NetImgui.
//
// Network exchange runs on NetImgui worker threads. update() is intentionally a
// main/render-thread operation because it converts pending texture commands into
// Dear ImGui texture objects belonging to the caller's current ImGui context.
class NetImguiHost {
public:
    explicit NetImguiHost(NetImguiHostConfig config = {});
    ~NetImguiHost();

    NetImguiHost(const NetImguiHost&) = delete;
    NetImguiHost& operator=(const NetImguiHost&) = delete;

    // Process received texture/draw data and request a new remote UI frame.
    // The caller must make the remote renderer's ImGuiContext current first.
    // width/height and dpiScale are sent to the client in CmdInput so its
    // ImGuiIO is built for the actual Reg Raw presentation surface.
    void update(
        std::uint32_t width,
        std::uint32_t height,
        bool active = true,
        float dpiScale = 1.0F);

    // Pointer remains owned by NetImgui and is valid until a later update() for
    // this client or host shutdown. Currently Reg exposes the first connected
    // client; maxClients is retained for the underlying server lifecycle.
    ImDrawData* drawData() const noexcept;

    NetImguiHostStatus status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reg::remote
