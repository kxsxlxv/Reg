#pragma once
#include <cstdint>

namespace reg::vulkan {

// Called after parsing command line and before creating Vulkan renderers.
// Has no effect in builds without REG_ENABLE_NETIMGUI_REMOTE.
void configureNetImguiServer(std::uint16_t port, bool enabled);

} // namespace reg::vulkan
