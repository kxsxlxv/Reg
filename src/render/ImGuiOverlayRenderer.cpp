#include "render/ImGuiOverlayRenderer.hpp"

#include "render/ImGuiTheme.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace reg::render {
namespace {

ImU32 toImColor(Color color) {
    const auto clamp = [](float value) {
        return std::clamp(value, 0.0F, 1.0F);
    };

    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        clamp(color.r),
        clamp(color.g),
        clamp(color.b),
        clamp(color.a)));
}

ImVec2 toImVec(video::Vec2 value) {
    return ImVec2(value.x, value.y);
}

void drawPatternedLine(
    ImDrawList* drawList,
    video::Vec2 from,
    video::Vec2 to,
    const LineStyle& style) {
    const ImU32 color = toImColor(style.color);

    if (style.pattern == LinePattern::Solid) {
        drawList->AddLine(
            toImVec(from),
            toImVec(to),
            color,
            style.thickness);
        return;
    }

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.001F) {
        return;
    }

    const float ux = dx / length;
    const float uy = dy / length;
    const float dash = std::max(style.dashLengthPx, 1.0F);
    const float gap = std::max(style.gapLengthPx, 1.0F);

    float cursor = 0.0F;
    bool drawDotNext = false;

    while (cursor < length) {
        float segment = dash;
        if (style.pattern == LinePattern::DashDot && drawDotNext) {
            segment = std::max(style.thickness, 1.0F);
        }

        const float end = std::min(cursor + segment, length);
        drawList->AddLine(
            ImVec2(from.x + ux * cursor, from.y + uy * cursor),
            ImVec2(from.x + ux * end, from.y + uy * end),
            color,
            style.thickness);

        cursor = end + gap;
        if (style.pattern == LinePattern::DashDot) {
            drawDotNext = !drawDotNext;
        }
    }
}

void drawRectPattern(
    ImDrawList* drawList,
    const RectPrimitive& rect) {
    if (rect.style.pattern == LinePattern::Solid) {
        drawList->AddRect(
            ImVec2(rect.rect.x, rect.rect.y),
            ImVec2(
                rect.rect.x + rect.rect.width,
                rect.rect.y + rect.rect.height),
            toImColor(rect.style.color),
            0.0F,
            0,
            rect.style.thickness);
        return;
    }

    const video::Vec2 a{rect.rect.x, rect.rect.y};
    const video::Vec2 b{rect.rect.x + rect.rect.width, rect.rect.y};
    const video::Vec2 c{
        rect.rect.x + rect.rect.width,
        rect.rect.y + rect.rect.height};
    const video::Vec2 d{rect.rect.x, rect.rect.y + rect.rect.height};

    drawPatternedLine(drawList, a, b, rect.style);
    drawPatternedLine(drawList, b, c, rect.style);
    drawPatternedLine(drawList, c, d, rect.style);
    drawPatternedLine(drawList, d, a, rect.style);
}

void drawScenePrimitive(
    ImDrawList* drawList,
    const OverlayPrimitive& primitive) {
    std::visit(
        [drawList](const auto& value) {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, LinePrimitive>) {
                drawPatternedLine(drawList, value.from, value.to, value.style);
            } else if constexpr (std::is_same_v<T, PolylinePrimitive>) {
                if (value.points.size() < 2) {
                    return;
                }

                if (value.style.pattern == LinePattern::Solid) {
                    std::vector<ImVec2> points;
                    points.reserve(value.points.size());
                    for (const auto& point : value.points) {
                        points.push_back(toImVec(point));
                    }
                    drawList->AddPolyline(
                        points.data(),
                        static_cast<int>(points.size()),
                        toImColor(value.style.color),
                        value.closed ? ImDrawFlags_Closed : ImDrawFlags_None,
                        value.style.thickness);
                } else {
                    for (std::size_t i = 1; i < value.points.size(); ++i) {
                        drawPatternedLine(
                            drawList,
                            value.points[i - 1],
                            value.points[i],
                            value.style);
                    }
                    if (value.closed) {
                        drawPatternedLine(
                            drawList,
                            value.points.back(),
                            value.points.front(),
                            value.style);
                    }
                }
            } else if constexpr (std::is_same_v<T, RectPrimitive>) {
                drawRectPattern(drawList, value);
            } else if constexpr (std::is_same_v<T, FilledRectPrimitive>) {
                drawList->AddRectFilled(
                    ImVec2(value.rect.x, value.rect.y),
                    ImVec2(
                        value.rect.x + value.rect.width,
                        value.rect.y + value.rect.height),
                    toImColor(value.color));
            } else if constexpr (std::is_same_v<T, CirclePrimitive>) {
                drawList->AddCircle(
                    toImVec(value.center),
                    value.radiusPx,
                    toImColor(value.style.color),
                    0,
                    value.style.thickness);
            } else if constexpr (std::is_same_v<T, CrosshairPrimitive>) {
                drawPatternedLine(
                    drawList,
                    video::Vec2{
                        value.center.x - value.armLengthPx,
                        value.center.y},
                    video::Vec2{
                        value.center.x + value.armLengthPx,
                        value.center.y},
                    value.style);
                drawPatternedLine(
                    drawList,
                    video::Vec2{
                        value.center.x,
                        value.center.y - value.armLengthPx},
                    video::Vec2{
                        value.center.x,
                        value.center.y + value.armLengthPx},
                    value.style);
            } else if constexpr (std::is_same_v<T, TextPrimitive>) {
                ImFont* font = ImGui::GetFont();
                const ImVec2 size = font->CalcTextSizeA(
                    value.fontSizePx,
                    FLT_MAX,
                    0.0F,
                    value.utf8.c_str());

                const ImVec2 backgroundMin(
                    value.position.x,
                    value.position.y);
                const ImVec2 backgroundMax(
                    value.position.x + size.x + value.paddingPx * 2.0F,
                    value.position.y + size.y + value.paddingPx * 2.0F);

                drawList->AddRectFilled(
                    backgroundMin,
                    backgroundMax,
                    toImColor(value.backgroundColor));
                drawList->AddText(
                    font,
                    value.fontSizePx,
                    ImVec2(
                        value.position.x + value.paddingPx,
                        value.position.y + value.paddingPx),
                    toImColor(value.textColor),
                    value.utf8.c_str());
            }
        },
        primitive);
}

} // namespace

struct ImGuiOverlayRenderer::Impl {
    explicit Impl(const vulkan::VulkanContext& context)
        : vulkan(context) {}

    const vulkan::VulkanContext& vulkan;
    ImGuiContext* context{nullptr};
    ImDrawData* drawData{nullptr};

    bool backendInitialized{false};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkSwapchainKHR observedSwapchain{VK_NULL_HANDLE};
    VkFormat observedFormat{VK_FORMAT_UNDEFINED};
    std::uint32_t observedImageCount{0};
    VkFormat pipelineFormat{VK_FORMAT_UNDEFINED};
    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};

    float contentScale{1.0F};
    std::chrono::steady_clock::time_point lastFrameTime{
        std::chrono::steady_clock::now()};

    void setCurrentContext() const {
        ImGui::SetCurrentContext(context);
    }

    void createDescriptorPool() {
        if (descriptorPool != VK_NULL_HANDLE) {
            return;
        }

        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
        };

        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 128;
        info.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        info.pPoolSizes = sizes;

        vulkan::checkVk(
            vkCreateDescriptorPool(
                vulkan.device(),
                &info,
                nullptr,
                &descriptorPool),
            "vkCreateDescriptorPool(ImGui overlay)");
    }

    void shutdownBackend() {
        if (backendInitialized) {
            setCurrentContext();
            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        }
        drawData = nullptr;

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(
                vulkan.device(),
                descriptorPool,
                nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }

        observedSwapchain = VK_NULL_HANDLE;
        observedFormat = VK_FORMAT_UNDEFINED;
        observedImageCount = 0;
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
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        }

        backendInitialized = true;
        observedSwapchain = swapchain.handle();
        observedFormat = swapchain.format();
        observedImageCount =
            static_cast<std::uint32_t>(swapchain.imageCount());
    }

    void ensureBackend(const vulkan::Swapchain& swapchain) {
        if (backendInitialized &&
            observedSwapchain == swapchain.handle() &&
            observedFormat == swapchain.format() &&
            observedImageCount ==
                static_cast<std::uint32_t>(swapchain.imageCount())) {
            return;
        }

        if (backendInitialized) {
            vulkan::checkVk(
                vkQueueWaitIdle(vulkan.graphicsQueue().handle),
                "vkQueueWaitIdle before ImGui backend rebuild");
            shutdownBackend();
        }
        initializeBackend(swapchain);
    }

    ImDrawList* beginFrame(const vulkan::Swapchain& swapchain) {
        setCurrentContext();
        ensureBackend(swapchain);

        const VkExtent2D extent = swapchain.extent();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(
            static_cast<float>(extent.width),
            static_cast<float>(extent.height));
        io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> delta = now - lastFrameTime;
        lastFrameTime = now;
        io.DeltaTime = std::max(delta.count(), 1.0F / 1000.0F);

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        return ImGui::GetBackgroundDrawList();
    }

    void endFrame() {
        ImGui::Render();
        drawData = ImGui::GetDrawData();
    }

    void drawNoSignal(
        const vulkan::Swapchain& swapchain,
        const char* subtitle) {
        const char* safeSubtitle =
            subtitle != nullptr ? subtitle : "СИГНАЛ НЕДОСТУПЕН";

        ImDrawList* drawList = beginFrame(swapchain);
        const VkExtent2D extent = swapchain.extent();
        ImFont* font = ImGui::GetFont();

        const float scale = contentScale;
        const float iconSize = 46.0F * scale;
        const float titleSize = 30.0F * scale;
        const float subtitleSize = 15.0F * scale;
        const float gap = 12.0F * scale;
        const float paddingX = 34.0F * scale;
        const float paddingY = 26.0F * scale;

        const ImVec2 iconExtent = font->CalcTextSizeA(
            iconSize, FLT_MAX, 0.0F, kIconVideoOff);
        const ImVec2 titleExtent = font->CalcTextSizeA(
            titleSize, FLT_MAX, 0.0F, "НЕТ СИГНАЛА");
        const ImVec2 subtitleExtent = font->CalcTextSizeA(
            subtitleSize,
            FLT_MAX,
            0.0F,
            safeSubtitle);

        const float contentWidth = std::max(
            {iconExtent.x, titleExtent.x, subtitleExtent.x});
        const float contentHeight =
            iconExtent.y + gap + titleExtent.y +
            gap * 0.55F + subtitleExtent.y;
        const float panelWidth = contentWidth + paddingX * 2.0F;
        const float panelHeight = contentHeight + paddingY * 2.0F;

        const ImVec2 panelMin(
            (static_cast<float>(extent.width) - panelWidth) * 0.5F,
            (static_cast<float>(extent.height) - panelHeight) * 0.5F);
        const ImVec2 panelMax(
            panelMin.x + panelWidth,
            panelMin.y + panelHeight);

        const ImU32 panelColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.102F, 0.114F, 0.137F, 0.96F));
        const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.290F, 0.565F, 0.851F, 0.85F));
        const ImU32 iconColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.290F, 0.565F, 0.851F, 1.0F));
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.878F, 0.878F, 0.878F, 1.0F));
        const ImU32 mutedColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.439F, 0.439F, 0.439F, 1.0F));

        drawList->AddRectFilled(panelMin, panelMax, panelColor);
        drawList->AddRect(panelMin, panelMax, borderColor, 0.0F, 0, 1.0F * scale);

        float y = panelMin.y + paddingY;
        drawList->AddText(
            font,
            iconSize,
            ImVec2(
                panelMin.x + (panelWidth - iconExtent.x) * 0.5F,
                y),
            iconColor,
            kIconVideoOff);
        y += iconExtent.y + gap;

        drawList->AddText(
            font,
            titleSize,
            ImVec2(
                panelMin.x + (panelWidth - titleExtent.x) * 0.5F,
                y),
            textColor,
            "НЕТ СИГНАЛА");
        y += titleExtent.y + gap * 0.55F;

        drawList->AddText(
            font,
            subtitleSize,
            ImVec2(
                panelMin.x + (panelWidth - subtitleExtent.x) * 0.5F,
                y),
            mutedColor,
            safeSubtitle);

        endFrame();
    }
};

ImGuiOverlayRenderer::ImGuiOverlayRenderer(
    const vulkan::VulkanContext& vulkan,
    const vulkan::Swapchain& initialSwapchain)
    : impl_(std::make_unique<Impl>(vulkan)) {
    IMGUI_CHECKVERSION();

    impl_->context = ImGui::CreateContext();
    if (impl_->context == nullptr) {
        throw std::runtime_error("ImGui::CreateContext failed");
    }

    impl_->setCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    impl_->contentScale = initialSwapchain.contentScale();
    configureRegImGuiTheme(impl_->contentScale);

    try {
        impl_->initializeBackend(initialSwapchain);
    } catch (...) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        throw;
    }
}

ImGuiOverlayRenderer::~ImGuiOverlayRenderer() {
    if (!impl_) {
        return;
    }

    if (impl_->vulkan.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl_->vulkan.device());
    }

    impl_->setCurrentContext();
    impl_->shutdownBackend();
    ImGui::DestroyContext(impl_->context);
    impl_->context = nullptr;
}

void ImGuiOverlayRenderer::prepare(
    const OverlayScene& scene,
    const vulkan::Swapchain& swapchain) {
    ImDrawList* drawList = impl_->beginFrame(swapchain);
    for (const OverlayPrimitive& primitive : scene.primitives) {
        drawScenePrimitive(drawList, primitive);
    }
    impl_->endFrame();
}

void ImGuiOverlayRenderer::prepareNoSignal(
    const vulkan::Swapchain& swapchain,
    const char* subtitle) {
    impl_->drawNoSignal(swapchain, subtitle);
}

void ImGuiOverlayRenderer::record(
    VkCommandBuffer commandBuffer,
    VkFormat colorAttachmentFormat,
    VkExtent2D framebufferExtent) {
    if (!impl_->backendInitialized || impl_->drawData == nullptr) {
        return;
    }

    if (colorAttachmentFormat != impl_->observedFormat) {
        throw std::runtime_error(
            "overlay prepared for a different color attachment format");
    }

    const int drawWidth = static_cast<int>(impl_->drawData->DisplaySize.x);
    const int drawHeight = static_cast<int>(impl_->drawData->DisplaySize.y);
    if (drawWidth != static_cast<int>(framebufferExtent.width) ||
        drawHeight != static_cast<int>(framebufferExtent.height)) {
        throw std::runtime_error(
            "overlay draw data extent does not match swapchain extent");
    }

    impl_->setCurrentContext();
    ImGui_ImplVulkan_RenderDrawData(impl_->drawData, commandBuffer);
}

} // namespace reg::render