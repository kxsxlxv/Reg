#include "render/RemoteImGuiRenderer.hpp"

#include "remote/NetImguiEmbeddedBridge.hpp"
#include "vulkan/BlankRenderer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace reg::render {

struct RemoteImGuiRenderer::Impl final : VideoOverlayRecorder {
    Impl(
        const vulkan::VulkanContext& context,
        remote::NetImguiHostConfig hostConfig)
        : vulkan(context),
          canvas(context),
          hostConfig_(hostConfig) {}

    const vulkan::VulkanContext& vulkan;
    vulkan::BlankRenderer canvas;
    remote::NetImguiHostConfig hostConfig_;
    std::unique_ptr<remote::NetImguiHost> host;
    ImGuiContext* context{nullptr};
    ImDrawData* drawData{nullptr};
    bool backendInitialized{false};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkFormat pipelineFormat{VK_FORMAT_UNDEFINED};
    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};

    void setCurrentContext() const {
        ImGui::SetCurrentContext(context);
    }

    void createDescriptorPool() {
        if (descriptorPool != VK_NULL_HANDLE) {
            return;
        }

        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
        };

        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 512;
        info.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        info.pPoolSizes = sizes;

        vulkan::checkVk(
            vkCreateDescriptorPool(
                vulkan.device(),
                &info,
                nullptr,
                &descriptorPool),
            "vkCreateDescriptorPool(remote ImGui)");
    }

    void initializeBackend(const vulkan::Swapchain& swapchain) {
        createDescriptorPool();
        pipelineFormat = swapchain.format();
        pipelineRenderingInfo = {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        pipelineRenderingInfo.colorAttachmentCount = 1;
        pipelineRenderingInfo.pColorAttachmentFormats = &pipelineFormat;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = vulkan.instance();
        initInfo.PhysicalDevice = vulkan.physicalDevice();
        initInfo.Device = vulkan.device();
        initInfo.QueueFamily = vulkan.graphicsQueue().familyIndex;
        initInfo.Queue = vulkan.graphicsQueue().handle;
        initInfo.DescriptorPool = descriptorPool;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = std::max(
            2U,
            static_cast<std::uint32_t>(swapchain.imageCount()));
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo =
            pipelineRenderingInfo;
        initInfo.UseDynamicRendering = true;
        initInfo.MinAllocationSize = 1024U * 1024U;

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error(
                "ImGui_ImplVulkan_Init(remote ImGui) failed");
        }
        backendInitialized = true;
    }

    void shutdownBackend() {
        if (backendInitialized) {
            setCurrentContext();
            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        }
        drawData = nullptr;
    }

    bool updateRemoteTextures() {
        bool changed = false;
        const auto textureCount = remote::netImguiServerTextureCount();
        for (std::size_t index = 0; index < textureCount; ++index) {
            ImTextureData* texture = remote::netImguiServerTexture(index);
            if (texture != nullptr && texture->Status != ImTextureStatus_OK &&
                texture->Status != ImTextureStatus_Destroyed) {
                ImGui_ImplVulkan_UpdateTexture(texture);
                changed = true;
            }
        }
        return changed;
    }

    void destroyRemoteGpuTextures() {
        if (!backendInitialized) {
            return;
        }

        setCurrentContext();
        const auto textureCount = remote::netImguiServerTextureCount();
        for (std::size_t index = 0; index < textureCount; ++index) {
            ImTextureData* texture = remote::netImguiServerTexture(index);
            if (texture == nullptr || texture->Status == ImTextureStatus_Destroyed) {
                continue;
            }

            texture->WantDestroyNextFrame = false;
            texture->UnusedFrames = std::numeric_limits<int>::max();
            texture->Status = ImTextureStatus_WantDestroy;
            ImGui_ImplVulkan_UpdateTexture(texture);
        }
    }

    void prepare(const vulkan::Swapchain& swapchain, bool active) {
        setCurrentContext();

        if (!backendInitialized) {
            initializeBackend(swapchain);
        }

        // A surface-format change requires pipeline recreation. Reg's current
        // swapchain recovery keeps the selected surface format stable, so skip a
        // frame rather than silently rendering with an incompatible pipeline.
        if (swapchain.format() != pipelineFormat) {
            drawData = nullptr;
            return;
        }

        const VkExtent2D extent = swapchain.extent();
        host->update(extent.width, extent.height, active);

        // Reconstructed NetImgui draw data does not originate from this local
        // context's ImGui::Render(), so drive remote texture uploads explicitly.
        // A second host update lets a frame deferred on WantCreate become visible
        // immediately after its texture reaches ImTextureStatus_OK.
        if (updateRemoteTextures()) {
            host->update(extent.width, extent.height, active);
        }

        drawData = host->drawData();
        if (drawData == nullptr) {
            return;
        }

        const auto width = static_cast<std::uint32_t>(
            std::max(drawData->DisplaySize.x, 0.0F));
        const auto height = static_cast<std::uint32_t>(
            std::max(drawData->DisplaySize.y, 0.0F));
        if (width != extent.width || height != extent.height) {
            // Resize is asynchronous across TCP. Treat the old-size frame as
            // unavailable so the caller can keep its local fallback visible.
            drawData = nullptr;
        }
    }

    void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) override {
        if (!backendInitialized || drawData == nullptr) {
            return;
        }
        if (colorAttachmentFormat != pipelineFormat) {
            return;
        }

        const auto width = static_cast<std::uint32_t>(
            std::max(drawData->DisplaySize.x, 0.0F));
        const auto height = static_cast<std::uint32_t>(
            std::max(drawData->DisplaySize.y, 0.0F));
        if (width != framebufferExtent.width ||
            height != framebufferExtent.height) {
            return;
        }

        setCurrentContext();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    }

    bool render(vulkan::Swapchain& swapchain, bool active) {
        prepare(swapchain, active);
        if (drawData == nullptr) {
            return false;
        }
        return canvas.render(swapchain, this);
    }
};

RemoteImGuiRenderer::RemoteImGuiRenderer(
    const vulkan::VulkanContext& vulkan,
    const vulkan::Swapchain& initialSwapchain,
    remote::NetImguiHostConfig hostConfig)
    : impl_(std::make_unique<Impl>(vulkan, hostConfig)) {
    IMGUI_CHECKVERSION();
    impl_->context = ImGui::CreateContext();
    if (impl_->context == nullptr) {
        throw std::runtime_error(
            "ImGui::CreateContext(remote ImGui) failed");
    }

    impl_->setCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    try {
        impl_->initializeBackend(initialSwapchain);
        impl_->host = std::make_unique<remote::NetImguiHost>(hostConfig);
    } catch (...) {
        if (impl_->vulkan.device() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(impl_->vulkan.device());
        }
        impl_->setCurrentContext();
        impl_->shutdownBackend();
        if (impl_->descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(
                impl_->vulkan.device(),
                impl_->descriptorPool,
                nullptr);
            impl_->descriptorPool = VK_NULL_HANDLE;
        }
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        throw;
    }
}

RemoteImGuiRenderer::~RemoteImGuiRenderer() {
    if (!impl_) {
        return;
    }

    if (impl_->vulkan.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl_->vulkan.device());
    }

    impl_->setCurrentContext();

    // Stop network producers first, then explicitly release every Vulkan texture
    // owned by remote NetImgui clients before shutting down the ImGui backend.
    impl_->host.reset();
    impl_->destroyRemoteGpuTextures();
    remote::destroyNetImguiServerTextures();
    impl_->shutdownBackend();

    if (impl_->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(
            impl_->vulkan.device(),
            impl_->descriptorPool,
            nullptr);
        impl_->descriptorPool = VK_NULL_HANDLE;
    }

    ImGui::DestroyContext(impl_->context);
    impl_->context = nullptr;
}

bool RemoteImGuiRenderer::render(
    vulkan::Swapchain& swapchain,
    bool active) {
    return impl_->render(swapchain, active);
}

remote::NetImguiHostStatus RemoteImGuiRenderer::status() const noexcept {
    return impl_->host != nullptr
        ? impl_->host->status()
        : remote::NetImguiHostStatus{};
}

} // namespace reg::render
