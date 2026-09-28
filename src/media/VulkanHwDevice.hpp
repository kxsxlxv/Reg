#pragma once

struct AVBufferRef;

#include <vector>

namespace reg::vulkan {
class VulkanContext;
}

namespace reg::media {

class VulkanHwDevice final {
public:
    explicit VulkanHwDevice(const vulkan::VulkanContext& vulkan);
    ~VulkanHwDevice();

    VulkanHwDevice(const VulkanHwDevice&) = delete;
    VulkanHwDevice& operator=(const VulkanHwDevice&) = delete;

    AVBufferRef* ref() const noexcept { return deviceRef_; }

private:
    AVBufferRef* deviceRef_{nullptr};
    std::vector<const char*> instanceExtensionPtrs_;
    std::vector<const char*> deviceExtensionPtrs_;
};

} // namespace reg::media
