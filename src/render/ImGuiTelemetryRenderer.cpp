#include "render/ImGuiTelemetryRenderer.hpp"

#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace reg::render {
namespace {

void checkVk(
    VkResult result,
    const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) +
            " failed with VkResult=" +
            std::to_string(result));
    }
}

std::optional<std::filesystem::path>
findFontPath() {
    if (const char* explicitPath =
            std::getenv("REG_FONT_PATH")) {
        const std::filesystem::path candidate(
            explicitPath);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

#ifdef _WIN32
    constexpr std::array<const char*, 3> candidates{
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
    };
#else
    constexpr std::array<const char*, 5> candidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    };
#endif

    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::path(candidate);
        }
    }

    return std::nullopt;
}

float ratePerSecond(
    std::uint64_t current,
    std::uint64_t previous,
    std::chrono::steady_clock::duration elapsed) {
    if (current < previous) {
        return 0.0F;
    }

    const double seconds =
        std::chrono::duration<double>(elapsed)
            .count();

    if (seconds <= 0.0) {
        return 0.0F;
    }

    return static_cast<float>(
        static_cast<double>(
            current - previous) /
        seconds);
}

void buildRateHistory(
    const telemetry::Snapshot& snapshot,
    std::vector<float>& rawFps,
    std::vector<float>& overlayFps) {
    rawFps.clear();
    overlayFps.clear();

    if (snapshot.samples.size() < 2) {
        return;
    }

    rawFps.reserve(
        snapshot.samples.size() - 1);
    overlayFps.reserve(
        snapshot.samples.size() - 1);

    for (std::size_t i = 1;
         i < snapshot.samples.size();
         ++i) {
        const auto& previous =
            snapshot.samples[i - 1];
        const auto& current =
            snapshot.samples[i];

        const auto elapsed =
            current.time - previous.time;

        rawFps.push_back(
            ratePerSecond(
                current.counters.rawPresentedFrames,
                previous.counters.rawPresentedFrames,
                elapsed));

        overlayFps.push_back(
            ratePerSecond(
                current.counters.overlayPresentedFrames,
                previous.counters.overlayPresentedFrames,
                elapsed));
    }
}

void drawStatus(
    const telemetry::Counters& counters) {
    if (ImGui::BeginTable(
            "status",
            4,
            ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchSame)) {
        const auto cell =
            [](const char* label,
               const char* value) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label);
                ImGui::TextUnformatted(value);
            };

        char buffer[64]{};

        cell(
            "RTSP",
            counters.rtspConnected
                ? "CONNECTED"
                : "DISCONNECTED");

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.decoderSessions));
        cell("Sessions", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.reconnects));
        cell("Reconnects", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.decodedFrames));
        cell("Decoded", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%zu",
            counters.overlayBufferDepth);
        cell("Overlay buffer", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%zu",
            counters.metadataStoreDepth);
        cell("Metadata store", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.overlayMissingMetadataDrops));
        cell("Overlay drops", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.metadataSequenceGaps));
        cell("UDP gaps", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%zu",
            counters.recorderQueueDepth);
        cell("Recorder queue", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.recorderQueueDrops));
        cell("Recorder drops", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.recorderMetadataQueueDrops));
        cell("Metadata rec drops", buffer);

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(
                counters.recorderFailures));
        cell("Recorder failures", buffer);

        ImGui::EndTable();
    }
}

void drawTargets(
    const std::vector<telemetry::Target>& targets) {
    if (!ImGui::BeginTable(
            "targets",
            7,
            ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY,
            ImVec2(0.0F, 220.0F))) {
        return;
    }

    ImGui::TableSetupColumn("ID");
    ImGui::TableSetupColumn("Class");
    ImGui::TableSetupColumn("Conf");
    ImGui::TableSetupColumn("X");
    ImGui::TableSetupColumn("Y");
    ImGui::TableSetupColumn("W");
    ImGui::TableSetupColumn("H");
    ImGui::TableHeadersRow();

    for (const auto& target : targets) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text(
            "%llu",
            static_cast<unsigned long long>(
                target.id));

        ImGui::TableNextColumn();
        ImGui::Text(
            "%u",
            static_cast<unsigned>(
                target.classId));

        ImGui::TableNextColumn();
        ImGui::Text(
            "%.1f%%",
            static_cast<double>(
                target.confidence * 100.0F));

        ImGui::TableNextColumn();
        ImGui::Text("%.3f", static_cast<double>(target.bbox.x));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", static_cast<double>(target.bbox.y));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", static_cast<double>(target.bbox.width));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", static_cast<double>(target.bbox.height));
    }

    ImGui::EndTable();
}

void drawEvents(
    const std::vector<telemetry::Event>& events,
    std::chrono::steady_clock::time_point now) {
    if (!ImGui::BeginChild(
            "events",
            ImVec2(0.0F, 0.0F),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    for (const auto& event : events) {
        const double ageSeconds =
            std::max(
                0.0,
                std::chrono::duration<double>(
                    now - event.time)
                    .count());

        ImGui::Text(
            "-%7.2fs  %-5s  %s",
            ageSeconds,
            telemetry::toString(
                event.severity),
            event.message.c_str());
    }

    ImGui::EndChild();
}

void buildDashboard(
    const telemetry::Snapshot& snapshot,
    VkExtent2D extent,
    std::vector<float>& rawFps,
    std::vector<float>& overlayFps) {
    buildRateHistory(
        snapshot,
        rawFps,
        overlayFps);

    ImGui::SetNextWindowPos(
        ImVec2(0.0F, 0.0F),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(
            static_cast<float>(extent.width),
            static_cast<float>(extent.height)),
        ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin(
            "Reg Telemetry",
            nullptr,
            flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(
        "Reg / Telemetry & Blackbox");
    ImGui::Separator();

    drawStatus(snapshot.counters);

    ImGui::SeparatorText("Performance");

    if (!rawFps.empty()) {
        ImGui::PlotLines(
            "Raw FPS",
            rawFps.data(),
            static_cast<int>(
                rawFps.size()),
            0,
            nullptr,
            0.0F,
            144.0F,
            ImVec2(0.0F, 90.0F));
    } else {
        ImGui::TextUnformatted(
            "Raw FPS: collecting samples...");
    }

    if (!overlayFps.empty()) {
        ImGui::PlotLines(
            "Overlay FPS",
            overlayFps.data(),
            static_cast<int>(
                overlayFps.size()),
            0,
            nullptr,
            0.0F,
            144.0F,
            ImVec2(0.0F, 90.0F));
    }

    ImGui::SeparatorText("Current targets");
    drawTargets(snapshot.targets);

    ImGui::SeparatorText("Events / last 5 minutes");
    drawEvents(
        snapshot.events,
        std::chrono::steady_clock::now());

    ImGui::End();
}

} // namespace

struct ImGuiTelemetryRenderer::Impl {
    static constexpr std::size_t kFramesInFlight = 2;

    struct FrameSlot {
        VkCommandBuffer commandBuffer{
            VK_NULL_HANDLE};
        VkSemaphore imageAvailable{
            VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
    };

    explicit Impl(
        const vulkan::VulkanContext& context)
        : vulkan(context) {}

    const vulkan::VulkanContext& vulkan;

    ImGuiContext* context{nullptr};
    bool backendInitialized{false};
    VkDescriptorPool descriptorPool{
        VK_NULL_HANDLE};

    VkFormat pipelineFormat{
        VK_FORMAT_UNDEFINED};
    VkPipelineRenderingCreateInfoKHR
        pipelineRenderingInfo{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};

    VkSwapchainKHR observedSwapchain{
        VK_NULL_HANDLE};
    VkFormat observedFormat{
        VK_FORMAT_UNDEFINED};
    std::uint32_t observedImageCount{0};

    VkCommandPool commandPool{
        VK_NULL_HANDLE};
    std::array<FrameSlot, kFramesInFlight>
        frameSlots{};
    std::size_t nextFrameSlot{0};

    std::vector<VkImageLayout>
        swapchainImageLayouts;
    std::vector<VkSemaphore>
        renderFinishedSemaphores;

    std::chrono::steady_clock::time_point
        lastFrameTime{
            std::chrono::steady_clock::now()};

    std::vector<float> rawFps;
    std::vector<float> overlayFps;

    void setCurrentContext() const {
        ImGui::SetCurrentContext(context);
    }

    void createCommandResources() {
        VkCommandPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex =
            vulkan.graphicsQueue().familyIndex;

        checkVk(
            vkCreateCommandPool(
                vulkan.device(),
                &poolInfo,
                nullptr,
                &commandPool),
            "vkCreateCommandPool(telemetry)");

        std::array<
            VkCommandBuffer,
            kFramesInFlight>
            commandBuffers{};

        VkCommandBufferAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = commandPool;
        allocateInfo.level =
            VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount =
            static_cast<std::uint32_t>(
                commandBuffers.size());

        checkVk(
            vkAllocateCommandBuffers(
                vulkan.device(),
                &allocateInfo,
                commandBuffers.data()),
            "vkAllocateCommandBuffers(telemetry)");

        for (std::size_t i = 0;
             i < frameSlots.size();
             ++i) {
            frameSlots[i].commandBuffer =
                commandBuffers[i];

            VkSemaphoreCreateInfo semaphoreInfo{
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

            checkVk(
                vkCreateSemaphore(
                    vulkan.device(),
                    &semaphoreInfo,
                    nullptr,
                    &frameSlots[i].imageAvailable),
                "vkCreateSemaphore(telemetry imageAvailable)");

            VkFenceCreateInfo fenceInfo{
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags =
                VK_FENCE_CREATE_SIGNALED_BIT;

            checkVk(
                vkCreateFence(
                    vulkan.device(),
                    &fenceInfo,
                    nullptr,
                    &frameSlots[i].fence),
                "vkCreateFence(telemetry)");
        }
    }

    void destroySwapchainResources() {
        for (VkSemaphore semaphore :
             renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    vulkan.device(),
                    semaphore,
                    nullptr);
            }
        }
        renderFinishedSemaphores.clear();
        swapchainImageLayouts.clear();
        observedSwapchain =
            VK_NULL_HANDLE;
    }

    void destroyCommandResources() {
        destroySwapchainResources();

        for (auto& slot : frameSlots) {
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(
                    vulkan.device(),
                    slot.fence,
                    nullptr);
                slot.fence =
                    VK_NULL_HANDLE;
            }

            if (slot.imageAvailable !=
                VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    vulkan.device(),
                    slot.imageAvailable,
                    nullptr);
                slot.imageAvailable =
                    VK_NULL_HANDLE;
            }
        }

        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(
                vulkan.device(),
                commandPool,
                nullptr);
            commandPool = VK_NULL_HANDLE;
        }
    }

    void createDescriptorPool() {
        if (descriptorPool !=
            VK_NULL_HANDLE) {
            return;
        }

        const VkDescriptorPoolSize sizes[]{
            {
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                64,
            },
            {
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                64,
            },
            {
                VK_DESCRIPTOR_TYPE_SAMPLER,
                16,
            },
        };

        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags =
            VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 128;
        info.poolSizeCount =
            static_cast<std::uint32_t>(
                std::size(sizes));
        info.pPoolSizes = sizes;

        checkVk(
            vkCreateDescriptorPool(
                vulkan.device(),
                &info,
                nullptr,
                &descriptorPool),
            "vkCreateDescriptorPool(ImGui telemetry)");
    }

    void shutdownBackend() {
        if (backendInitialized) {
            setCurrentContext();
            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        }

        if (descriptorPool !=
            VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(
                vulkan.device(),
                descriptorPool,
                nullptr);
            descriptorPool =
                VK_NULL_HANDLE;
        }

        observedFormat =
            VK_FORMAT_UNDEFINED;
        observedImageCount = 0;
    }

    void initializeBackend(
        const vulkan::Swapchain& swapchain) {
        createDescriptorPool();

        pipelineFormat = swapchain.format();

        pipelineRenderingInfo = {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        pipelineRenderingInfo.colorAttachmentCount = 1;
        pipelineRenderingInfo.pColorAttachmentFormats =
            &pipelineFormat;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion =
            VK_API_VERSION_1_3;
        initInfo.Instance =
            vulkan.instance();
        initInfo.PhysicalDevice =
            vulkan.physicalDevice();
        initInfo.Device =
            vulkan.device();
        initInfo.QueueFamily =
            vulkan.graphicsQueue().familyIndex;
        initInfo.Queue =
            vulkan.graphicsQueue().handle;
        initInfo.DescriptorPool =
            descriptorPool;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount =
            std::max(
                2U,
                static_cast<std::uint32_t>(
                    swapchain.imageCount()));
        initInfo.PipelineInfoMain.MSAASamples =
            VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo =
            pipelineRenderingInfo;
        initInfo.UseDynamicRendering = true;
        initInfo.MinAllocationSize =
            1024U * 1024U;

        if (!ImGui_ImplVulkan_Init(
                &initInfo)) {
            throw std::runtime_error(
                "ImGui_ImplVulkan_Init(telemetry) failed");
        }

        backendInitialized = true;
        observedFormat =
            swapchain.format();
        observedImageCount =
            static_cast<std::uint32_t>(
                swapchain.imageCount());
    }

    void ensureBackend(
        const vulkan::Swapchain& swapchain) {
        const bool needsRebuild =
            !backendInitialized ||
            observedFormat !=
                swapchain.format() ||
            observedImageCount !=
                static_cast<std::uint32_t>(
                    swapchain.imageCount());

        if (!needsRebuild) {
            return;
        }

        checkVk(
            vkQueueWaitIdle(
                vulkan.graphicsQueue().handle),
            "vkQueueWaitIdle(telemetry backend rebuild)");

        shutdownBackend();
        initializeBackend(swapchain);
    }

    void syncSwapchain(
        const vulkan::Swapchain& swapchain) {
        if (observedSwapchain ==
                swapchain.handle() &&
            swapchainImageLayouts.size() ==
                swapchain.imageCount() &&
            renderFinishedSemaphores.size() ==
                swapchain.imageCount()) {
            return;
        }

        checkVk(
            vkQueueWaitIdle(
                vulkan.graphicsQueue().handle),
            "vkQueueWaitIdle(telemetry swapchain sync)");

        if (vulkan.presentQueue().handle !=
            vulkan.graphicsQueue().handle) {
            checkVk(
                vkQueueWaitIdle(
                    vulkan.presentQueue().handle),
                "vkQueueWaitIdle(telemetry present sync)");
        }

        destroySwapchainResources();

        observedSwapchain =
            swapchain.handle();

        swapchainImageLayouts.assign(
            swapchain.imageCount(),
            VK_IMAGE_LAYOUT_UNDEFINED);

        renderFinishedSemaphores.resize(
            swapchain.imageCount(),
            VK_NULL_HANDLE);

        VkSemaphoreCreateInfo info{
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        for (auto& semaphore :
             renderFinishedSemaphores) {
            checkVk(
                vkCreateSemaphore(
                    vulkan.device(),
                    &info,
                    nullptr,
                    &semaphore),
                "vkCreateSemaphore(telemetry renderFinished)");
        }
    }

    void beginUiFrame(
        const telemetry::Snapshot& snapshot,
        VkExtent2D extent) {
        setCurrentContext();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(
            static_cast<float>(extent.width),
            static_cast<float>(extent.height));
        io.DisplayFramebufferScale =
            ImVec2(1.0F, 1.0F);

        const auto now =
            std::chrono::steady_clock::now();
        const std::chrono::duration<float>
            delta =
                now - lastFrameTime;
        lastFrameTime = now;

        io.DeltaTime =
            std::max(
                delta.count(),
                1.0F / 1000.0F);

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        buildDashboard(
            snapshot,
            extent,
            rawFps,
            overlayFps);

        ImGui::Render();
    }

    static void recordToColor(
        VkCommandBuffer commandBuffer,
        VkImage image,
        VkImageLayout oldLayout) {
        VkImageMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = oldLayout;
        barrier.newLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers =
            &barrier;

        vkCmdPipelineBarrier2(
            commandBuffer,
            &dependency);
    }

    static void recordToPresent(
        VkCommandBuffer commandBuffer,
        VkImage image) {
        VkImageMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = 0;
        barrier.oldLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout =
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers =
            &barrier;

        vkCmdPipelineBarrier2(
            commandBuffer,
            &dependency);
    }
};

ImGuiTelemetryRenderer::ImGuiTelemetryRenderer(
    const vulkan::VulkanContext& vulkan,
    const vulkan::Swapchain& initialSwapchain)
    : impl_(std::make_unique<Impl>(vulkan)) {
    IMGUI_CHECKVERSION();

    impl_->context =
        ImGui::CreateContext();

    if (impl_->context == nullptr) {
        throw std::runtime_error(
            "ImGui::CreateContext(telemetry) failed");
    }

    impl_->setCurrentContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    if (const auto fontPath =
            findFontPath()) {
        const ImWchar* ranges =
            io.Fonts->GetGlyphRangesCyrillic();

        if (io.Fonts->AddFontFromFileTTF(
                fontPath->string().c_str(),
                17.0F,
                nullptr,
                ranges) == nullptr) {
            io.Fonts->AddFontDefault();
        }
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    try {
        impl_->createCommandResources();
        impl_->initializeBackend(
            initialSwapchain);
        impl_->syncSwapchain(
            initialSwapchain);
    } catch (...) {
        impl_->destroyCommandResources();
        impl_->shutdownBackend();
        ImGui::DestroyContext(
            impl_->context);
        impl_->context = nullptr;
        throw;
    }
}

ImGuiTelemetryRenderer::~ImGuiTelemetryRenderer() {
    if (!impl_) {
        return;
    }

    if (impl_->vulkan.device() !=
        VK_NULL_HANDLE) {
        vkDeviceWaitIdle(
            impl_->vulkan.device());
    }

    impl_->setCurrentContext();
    impl_->destroyCommandResources();
    impl_->shutdownBackend();

    ImGui::DestroyContext(
        impl_->context);
    impl_->context = nullptr;
}

bool ImGuiTelemetryRenderer::render(
    const telemetry::Snapshot& snapshot,
    vulkan::Swapchain& swapchain) {
    Impl::FrameSlot& slot =
        impl_->frameSlots[
            impl_->nextFrameSlot];

    const VkResult fenceStatus =
        vkGetFenceStatus(
            impl_->vulkan.device(),
            slot.fence);

    if (fenceStatus == VK_NOT_READY) {
        return false;
    }

    checkVk(
        fenceStatus,
        "vkGetFenceStatus(telemetry)");

    impl_->ensureBackend(swapchain);
    impl_->syncSwapchain(swapchain);

    std::uint32_t imageIndex = 0;
    const vulkan::AcquireStatus acquireStatus =
        swapchain.acquire(
            slot.imageAvailable,
            imageIndex);

    if (acquireStatus ==
        vulkan::AcquireStatus::NotReady) {
        return false;
    }

    if (acquireStatus ==
        vulkan::AcquireStatus::Recreate) {
        static_cast<void>(
            swapchain.recreate());
        return false;
    }

    impl_->beginUiFrame(
        snapshot,
        swapchain.extent());

    checkVk(
        vkResetFences(
            impl_->vulkan.device(),
            1,
            &slot.fence),
        "vkResetFences(telemetry)");

    checkVk(
        vkResetCommandBuffer(
            slot.commandBuffer,
            0),
        "vkResetCommandBuffer(telemetry)");

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    checkVk(
        vkBeginCommandBuffer(
            slot.commandBuffer,
            &beginInfo),
        "vkBeginCommandBuffer(telemetry)");

    Impl::recordToColor(
        slot.commandBuffer,
        swapchain.image(imageIndex),
        impl_->swapchainImageLayouts.at(
            imageIndex));

    VkClearValue clear{};
    clear.color = {{
        0.035F,
        0.035F,
        0.045F,
        1.0F,
    }};

    VkRenderingAttachmentInfo colorAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView =
        swapchain.imageView(imageIndex);
    colorAttachment.imageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp =
        VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp =
        VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = clear;

    VkRenderingInfo renderingInfo{
        VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea.extent =
        swapchain.extent();
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments =
        &colorAttachment;

    vkCmdBeginRendering(
        slot.commandBuffer,
        &renderingInfo);

    impl_->setCurrentContext();
    ImGui_ImplVulkan_RenderDrawData(
        ImGui::GetDrawData(),
        slot.commandBuffer);

    vkCmdEndRendering(
        slot.commandBuffer);

    Impl::recordToPresent(
        slot.commandBuffer,
        swapchain.image(imageIndex));

    checkVk(
        vkEndCommandBuffer(
            slot.commandBuffer),
        "vkEndCommandBuffer(telemetry)");

    VkSemaphoreSubmitInfo imageWait{
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    imageWait.semaphore =
        slot.imageAvailable;
    imageWait.stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    const VkSemaphore renderFinished =
        impl_->renderFinishedSemaphores.at(
            imageIndex);

    VkSemaphoreSubmitInfo renderSignal{
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    renderSignal.semaphore =
        renderFinished;
    renderSignal.stageMask =
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer =
        slot.commandBuffer;

    VkSubmitInfo2 submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos =
        &imageWait;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos =
        &commandInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos =
        &renderSignal;

    checkVk(
        vkQueueSubmit2(
            impl_->vulkan.graphicsQueue().handle,
            1,
            &submitInfo,
            slot.fence),
        "vkQueueSubmit2(telemetry)");

    impl_->swapchainImageLayouts.at(
        imageIndex) =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    const bool presentOk =
        swapchain.present(
            imageIndex,
            renderFinished);

    impl_->nextFrameSlot =
        (impl_->nextFrameSlot + 1) %
        impl_->frameSlots.size();

    if (!presentOk) {
        static_cast<void>(
            swapchain.recreate());
        return false;
    }

    return true;
}

} // namespace reg::render
