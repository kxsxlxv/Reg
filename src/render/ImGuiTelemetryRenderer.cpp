#include "render/ImGuiTelemetryRenderer.hpp"

#include "render/ImGuiTheme.hpp"
#include "render/VideoOverlayRecorder.hpp"
#include "vulkan/BlankRenderer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace reg::render {
namespace {

constexpr std::uint16_t kDetectorFlag = 0x0001U;
constexpr std::uint16_t kPropagatedFlag = 0x0002U;
constexpr double kPerformanceWindowSeconds = 60.0;

const ImVec4 kAccentBlue{0.290F, 0.565F, 0.851F, 1.000F};
const ImVec4 kSignalGreen{0.25F, 1.0F, 0.25F, 1.0F};
const ImVec4 kWarningAmber{1.0F, 0.72F, 0.12F, 1.0F};
const ImVec4 kErrorRed{1.0F, 0.35F, 0.32F, 1.0F};

struct RateHistory {
    std::vector<double> secondsAgo;
    std::vector<double> decodedFps;
    std::vector<double> rawFps;
    std::vector<double> overlayFps;
};

struct RateStats {
    double current{};
    double minimum{};
    double average{};
    double maximum{};
    bool valid{};
};

double ratePerSecond(
    std::uint64_t current,
    std::uint64_t previous,
    std::chrono::steady_clock::duration elapsed) {
    if (current < previous) {
        return 0.0;
    }
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0.0
        ? static_cast<double>(current - previous) / seconds
        : 0.0;
}

void buildRateHistory(
    const telemetry::Snapshot& snapshot,
    std::chrono::steady_clock::time_point now,
    RateHistory& history) {
    history.secondsAgo.clear();
    history.decodedFps.clear();
    history.rawFps.clear();
    history.overlayFps.clear();

    if (snapshot.samples.size() < 2U) {
        return;
    }

    const auto cutoff =
        now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>{kPerformanceWindowSeconds});

    const std::size_t reserveCount = snapshot.samples.size() - 1U;
    history.secondsAgo.reserve(reserveCount);
    history.decodedFps.reserve(reserveCount);
    history.rawFps.reserve(reserveCount);
    history.overlayFps.reserve(reserveCount);

    for (std::size_t i = 1; i < snapshot.samples.size(); ++i) {
        const auto& previous = snapshot.samples[i - 1U];
        const auto& current = snapshot.samples[i];
        if (current.time < cutoff) {
            continue;
        }

        const auto elapsed = current.time - previous.time;
        history.secondsAgo.push_back(
            std::chrono::duration<double>(current.time - now).count());
        history.decodedFps.push_back(ratePerSecond(
            current.counters.decodedFrames,
            previous.counters.decodedFrames,
            elapsed));
        history.rawFps.push_back(ratePerSecond(
            current.counters.rawPresentedFrames,
            previous.counters.rawPresentedFrames,
            elapsed));
        history.overlayFps.push_back(ratePerSecond(
            current.counters.overlayPresentedFrames,
            previous.counters.overlayPresentedFrames,
            elapsed));
    }
}

RateStats rateStats(const std::vector<double>& values) {
    RateStats stats{};
    if (values.empty()) {
        return stats;
    }

    double sum = 0.0;
    std::size_t count = 0U;
    stats.minimum = std::numeric_limits<double>::infinity();
    stats.maximum = -std::numeric_limits<double>::infinity();

    for (const double value : values) {
        if (!std::isfinite(value)) {
            continue;
        }
        stats.minimum = std::min(stats.minimum, value);
        stats.maximum = std::max(stats.maximum, value);
        sum += value;
        ++count;
    }

    if (count == 0U) {
        return {};
    }

    stats.current = values.back();
    stats.average = sum / static_cast<double>(count);
    stats.valid = true;
    return stats;
}

void drawRateStatsLine(
    const char* label,
    const ImVec4& color,
    const RateStats& stats) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (!stats.valid) {
        ImGui::Text("%-8s  --.- FPS", label);
    } else {
        ImGui::Text(
            "%-8s  %6.2f FPS    MIN %6.2f | AVG %6.2f | MAX %6.2f",
            label,
            stats.current,
            stats.minimum,
            stats.average,
            stats.maximum);
    }
    ImGui::PopStyleColor();
}

void statusRow(const char* label, const char* value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
}

void statusRowColored(
    const char* label,
    const ImVec4& color,
    const char* icon,
    const char* value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("%s  %s", icon, value);
    ImGui::PopStyleColor();
}

void drawStatus(const telemetry::Counters& counters) {
    if (!ImGui::BeginTable(
            "status",
            2,
            ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.20F);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.80F);

    statusRowColored(
        "RTSP",
        counters.rtspConnected ? kSignalGreen : kErrorRed,
        counters.rtspConnected ? kIconCheckCircle : kIconWarning,
        counters.rtspConnected ? "CONNECTED" : "DISCONNECTED");
    statusRowColored(
        "Video signal",
        counters.videoSignalPresent ? kSignalGreen : kWarningAmber,
        counters.videoSignalPresent ? kIconCheckCircle : kIconVideoOff,
        counters.videoSignalPresent ? "SIGNAL" : "NO SIGNAL");

    char buffer[128]{};
    if (counters.decodedFrames == 0U) {
        statusRow("Last decoded frame", "never");
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu ms ago",
            static_cast<unsigned long long>(counters.videoFrameAgeMs));
        statusRow("Last decoded frame", buffer);
    }

    const auto addU64 = [&](const char* label, std::uint64_t value) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(value));
        statusRow(label, buffer);
    };

    addU64("Sessions", counters.decoderSessions);
    addU64("Reconnects", counters.reconnects);
    addU64("Decoded", counters.decodedFrames);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu / %llu",
        static_cast<unsigned long long>(counters.rawPresentedFrames),
        static_cast<unsigned long long>(counters.overlayPresentedFrames));
    statusRow("Presented raw / overlay", buffer);

    addU64("Overlay exact-key drops", counters.overlayMissingMetadataDrops);
    addU64("UDP sequence gaps", counters.metadataSequenceGaps);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%zu / %zu",
        counters.overlayBufferDepth,
        counters.metadataStoreDepth);
    statusRow("Overlay / metadata depth", buffer);
    std::snprintf(buffer, sizeof(buffer), "%zu", counters.recorderQueueDepth);
    statusRow("Recorder queue", buffer);
    addU64("Recorder drops", counters.recorderQueueDrops);
    addU64("Recorder failures", counters.recorderFailures);

    ImGui::EndTable();
}

void drawPerformance(const RateHistory& history) {
    ImGui::TextDisabled(
        "Rolling 60 s, raw samples (no smoothing / interpolation)");

    drawRateStatsLine(
        "Decoded",
        kSignalGreen,
        rateStats(history.decodedFps));
    drawRateStatsLine(
        "Raw",
        kAccentBlue,
        rateStats(history.rawFps));
    drawRateStatsLine(
        "Overlay",
        kWarningAmber,
        rateStats(history.overlayFps));

    if (history.secondsAgo.empty()) {
        ImGui::TextDisabled("Collecting performance samples...");
        return;
    }

    if (!ImPlot::BeginPlot(
            "##fps",
            ImVec2(-1.0F, 250.0F),
            ImPlotFlags_NoTitle)) {
        return;
    }

    ImPlot::SetupAxis(
        ImAxis_X1,
        "seconds ago",
        ImPlotAxisFlags_NoMenus);
    ImPlot::SetupAxisLimits(
        ImAxis_X1,
        -kPerformanceWindowSeconds,
        0.0,
        ImPlotCond_Always);
    ImPlot::SetupAxis(
        ImAxis_Y1,
        "FPS",
        ImPlotAxisFlags_AutoFit |
            ImPlotAxisFlags_RangeFit |
            ImPlotAxisFlags_NoMenus);
    ImPlot::SetupLegend(ImPlotLocation_NorthEast);

    const int count = static_cast<int>(history.secondsAgo.size());

    ImPlotSpec decodedSpec;
    decodedSpec.LineColor = kSignalGreen;
    decodedSpec.LineWeight = 2.0F;
    ImPlot::PlotLine(
        "Decoded",
        history.secondsAgo.data(),
        history.decodedFps.data(),
        count,
        decodedSpec);

    ImPlotSpec rawSpec;
    rawSpec.LineColor = kAccentBlue;
    rawSpec.LineWeight = 2.0F;
    ImPlot::PlotLine(
        "Raw",
        history.secondsAgo.data(),
        history.rawFps.data(),
        count,
        rawSpec);

    ImPlotSpec overlaySpec;
    overlaySpec.LineColor = kWarningAmber;
    overlaySpec.LineWeight = 2.0F;
    ImPlot::PlotLine(
        "Overlay",
        history.secondsAgo.data(),
        history.overlayFps.data(),
        count,
        overlaySpec);

    if (ImPlot::IsPlotHovered()) {
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        const auto nearest = std::min_element(
            history.secondsAgo.begin(),
            history.secondsAgo.end(),
            [&](double lhs, double rhs) {
                return std::abs(lhs - mouse.x) <
                    std::abs(rhs - mouse.x);
            });

        if (nearest != history.secondsAgo.end()) {
            const std::size_t index = static_cast<std::size_t>(
                std::distance(history.secondsAgo.begin(), nearest));
            ImGui::BeginTooltip();
            ImGui::Text("t = %.2f s", history.secondsAgo[index]);
            ImGui::Separator();
            ImGui::TextColored(
                kSignalGreen,
                "Decoded  %.2f FPS",
                history.decodedFps[index]);
            ImGui::TextColored(
                kAccentBlue,
                "Raw      %.2f FPS",
                history.rawFps[index]);
            ImGui::TextColored(
                kWarningAmber,
                "Overlay  %.2f FPS",
                history.overlayFps[index]);
            ImGui::EndTooltip();
        }
    }

    ImPlot::EndPlot();
}

void drawTargets(const std::vector<telemetry::Target>& targets) {
    std::size_t detectorCount = 0U;
    std::size_t propagatedCount = 0U;
    for (const auto& target : targets) {
        if ((target.flags & kPropagatedFlag) != 0U) {
            ++propagatedCount;
        } else if ((target.flags & kDetectorFlag) != 0U) {
            ++detectorCount;
        }
    }

    ImGui::Text(
        "Targets: %zu   YOLO: %zu   OFA: %zu",
        targets.size(),
        detectorCount,
        propagatedCount);

    if (!ImGui::BeginTable(
            "targets",
            1,
            ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY,
            ImVec2(0.0F, 220.0F))) {
        return;
    }

    ImGui::TableSetupColumn("Target");
    ImGui::TableHeadersRow();
    for (const auto& target : targets) {
        const bool propagated = (target.flags & kPropagatedFlag) != 0U;
        const char* source = propagated
            ? "OFA"
            : ((target.flags & kDetectorFlag) != 0U ? "YOLO" : "CV");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            propagated ? kWarningAmber : kSignalGreen);
        ImGui::Text(
            "%s  ID %llu  C%u  %.1f%%   bbox [%.3f, %.3f, %.3f, %.3f]",
            source,
            static_cast<unsigned long long>(target.id),
            static_cast<unsigned>(target.classId),
            static_cast<double>(target.confidence * 100.0F),
            static_cast<double>(target.bbox.x),
            static_cast<double>(target.bbox.y),
            static_cast<double>(target.bbox.width),
            static_cast<double>(target.bbox.height));
        ImGui::PopStyleColor();
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
        const double ageSeconds = std::max(
            0.0,
            std::chrono::duration<double>(now - event.time).count());
        ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (event.severity == telemetry::Severity::Warning) {
            color = kWarningAmber;
        } else if (event.severity == telemetry::Severity::Error) {
            color = kErrorRed;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::Text(
            "-%7.2fs  %-5s  %s",
            ageSeconds,
            telemetry::toString(event.severity),
            event.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

} // namespace

struct ImGuiTelemetryRenderer::Impl final : VideoOverlayRecorder {
    explicit Impl(const vulkan::VulkanContext& context)
        : vulkan(context), canvas(context) {}

    const vulkan::VulkanContext& vulkan;
    vulkan::BlankRenderer canvas;
    ImGuiContext* context{nullptr};
    ImPlotContext* plotContext{nullptr};
    ImDrawData* drawData{nullptr};
    bool backendInitialized{false};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkSwapchainKHR observedSwapchain{VK_NULL_HANDLE};
    VkFormat observedFormat{VK_FORMAT_UNDEFINED};
    std::uint32_t observedImageCount{};
    VkFormat pipelineFormat{VK_FORMAT_UNDEFINED};
    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    std::chrono::steady_clock::time_point lastFrameTime{
        std::chrono::steady_clock::now()};
    RateHistory rateHistory;

    void setCurrentContext() const {
        ImGui::SetCurrentContext(context);
        ImPlot::SetCurrentContext(plotContext);
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
            "vkCreateDescriptorPool(ImPlot telemetry)");
    }

    void shutdownBackend() {
        if (backendInitialized) {
            setCurrentContext();
            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        }
        drawData = nullptr;
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(vulkan.device(), descriptorPool, nullptr);
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
            throw std::runtime_error(
                "ImGui_ImplVulkan_Init(ImPlot telemetry) failed");
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
                "vkQueueWaitIdle before telemetry backend rebuild");
            shutdownBackend();
        }
        initializeBackend(swapchain);
    }

    void buildDashboard(
        const telemetry::Snapshot& snapshot,
        VkExtent2D extent,
        std::chrono::steady_clock::time_point now) {
        buildRateHistory(snapshot, now, rateHistory);
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
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
        if (!ImGui::Begin("Reg Telemetry", nullptr, flags)) {
            ImGui::End();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, kAccentBlue);
        ImGui::TextUnformatted("REG / TELEMETRY & BLACKBOX");
        ImGui::PopStyleColor();
        ImGui::Separator();
        drawStatus(snapshot.counters);
        ImGui::SeparatorText("PERFORMANCE");
        drawPerformance(rateHistory);
        ImGui::SeparatorText("CURRENT TARGETS");
        drawTargets(snapshot.targets);
        ImGui::SeparatorText("EVENTS / LAST 5 MINUTES");
        drawEvents(snapshot.events, now);
        ImGui::End();
    }

    void prepare(
        const telemetry::Snapshot& snapshot,
        const vulkan::Swapchain& swapchain) {
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
        buildDashboard(snapshot, extent, now);
        ImGui::Render();
        drawData = ImGui::GetDrawData();
    }

    void record(
        VkCommandBuffer commandBuffer,
        VkFormat colorAttachmentFormat,
        VkExtent2D framebufferExtent) override {
        if (!backendInitialized || drawData == nullptr) {
            return;
        }
        if (colorAttachmentFormat != observedFormat) {
            throw std::runtime_error(
                "telemetry prepared for a different color attachment format");
        }
        if (static_cast<int>(drawData->DisplaySize.x) !=
                static_cast<int>(framebufferExtent.width) ||
            static_cast<int>(drawData->DisplaySize.y) !=
                static_cast<int>(framebufferExtent.height)) {
            throw std::runtime_error(
                "telemetry draw data extent does not match swapchain extent");
        }
        setCurrentContext();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    }

    bool render(
        const telemetry::Snapshot& snapshot,
        vulkan::Swapchain& swapchain) {
        prepare(snapshot, swapchain);
        return canvas.render(swapchain, this);
    }
};

ImGuiTelemetryRenderer::ImGuiTelemetryRenderer(
    const vulkan::VulkanContext& vulkan,
    const vulkan::Swapchain& initialSwapchain)
    : impl_(std::make_unique<Impl>(vulkan)) {
    IMGUI_CHECKVERSION();
    impl_->context = ImGui::CreateContext();
    if (impl_->context == nullptr) {
        throw std::runtime_error(
            "ImGui::CreateContext(telemetry) failed");
    }

    ImGui::SetCurrentContext(impl_->context);
    impl_->plotContext = ImPlot::CreateContext();
    if (impl_->plotContext == nullptr) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        throw std::runtime_error(
            "ImPlot::CreateContext(telemetry) failed");
    }

    impl_->setCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    configureRegImGuiTheme(initialSwapchain.contentScale());
    ImPlot::GetStyle().UseLocalTime = false;

    try {
        impl_->initializeBackend(initialSwapchain);
    } catch (...) {
        impl_->setCurrentContext();
        ImPlot::DestroyContext(impl_->plotContext);
        impl_->plotContext = nullptr;
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        throw;
    }
}

ImGuiTelemetryRenderer::~ImGuiTelemetryRenderer() {
    if (!impl_) {
        return;
    }
    if (impl_->vulkan.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl_->vulkan.device());
    }
    impl_->setCurrentContext();
    impl_->shutdownBackend();
    ImPlot::DestroyContext(impl_->plotContext);
    impl_->plotContext = nullptr;
    ImGui::DestroyContext(impl_->context);
    impl_->context = nullptr;
}

bool ImGuiTelemetryRenderer::render(
    const telemetry::Snapshot& snapshot,
    vulkan::Swapchain& swapchain) {
    return impl_->render(snapshot, swapchain);
}

} // namespace reg::render
