#include "media/VulkanHwDevice.hpp"

#include "media/FfmpegError.hpp"
#include "vulkan/VulkanContext.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>

namespace reg::media {

VulkanHwDevice::VulkanHwDevice(const vulkan::VulkanContext& vulkan) {
    deviceRef_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (deviceRef_ == nullptr) {
        throw std::runtime_error("av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN) failed");
    }

    AVHWDeviceContext* base = reinterpret_cast<AVHWDeviceContext*>(deviceRef_->data);
    auto* vkContext = reinterpret_cast<AVVulkanDeviceContext*>(base->hwctx);

    vkContext->get_proc_addr = vkGetInstanceProcAddr;
    vkContext->inst = vulkan.instance();
    vkContext->phys_dev = vulkan.physicalDevice();
    vkContext->act_dev = vulkan.device();
    vkContext->device_features = vulkan.enabledFeatures();
    vkContext->queue_flags = vulkan.queueCreateFlags();

    instanceExtensionPtrs_.reserve(vulkan.enabledInstanceExtensions().size());
    for (const auto& extension : vulkan.enabledInstanceExtensions()) {
        instanceExtensionPtrs_.push_back(extension.c_str());
    }
    vkContext->enabled_inst_extensions = instanceExtensionPtrs_.data();
    vkContext->nb_enabled_inst_extensions = static_cast<int>(instanceExtensionPtrs_.size());

    deviceExtensionPtrs_.reserve(vulkan.enabledDeviceExtensions().size());
    for (const auto& extension : vulkan.enabledDeviceExtensions()) {
        deviceExtensionPtrs_.push_back(extension.c_str());
    }
    vkContext->enabled_dev_extensions = deviceExtensionPtrs_.data();
    vkContext->nb_enabled_dev_extensions = static_cast<int>(deviceExtensionPtrs_.size());

    auto addQueueFamily = [vkContext](const vulkan::QueueInfo& queue) {
        for (int i = 0; i < vkContext->nb_qf; ++i) {
            if (vkContext->qf[i].idx == static_cast<int>(queue.familyIndex)) {
                const auto flags = static_cast<VkQueueFlags>(vkContext->qf[i].flags) | queue.flags;
                vkContext->qf[i].flags = static_cast<VkQueueFlagBits>(flags);
                const auto videoCaps =
                    static_cast<VkVideoCodecOperationFlagsKHR>(vkContext->qf[i].video_caps) |
                    queue.videoCodecOperations;
                vkContext->qf[i].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(videoCaps);
                return;
            }
        }

        if (vkContext->nb_qf >= static_cast<int>(std::size(vkContext->qf))) {
            throw std::runtime_error("Too many Vulkan queue families for AVVulkanDeviceContext");
        }

        auto& family = vkContext->qf[vkContext->nb_qf++];
        family.idx = static_cast<int>(queue.familyIndex);
        family.num = 1;
        family.flags = static_cast<VkQueueFlagBits>(queue.flags);
        family.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(queue.videoCodecOperations);
    };

    // Prefer the dedicated video queue, then expose graphics if it is distinct.
    addQueueFamily(vulkan.videoDecodeQueue());
    addQueueFamily(vulkan.graphicsQueue());

    const int result = av_hwdevice_ctx_init(deviceRef_);
    if (result < 0) {
        av_buffer_unref(&deviceRef_);
        throwFfmpegError("av_hwdevice_ctx_init(Vulkan)", result);
    }
}

VulkanHwDevice::~VulkanHwDevice() {
    av_buffer_unref(&deviceRef_);
}

} // namespace reg::media
