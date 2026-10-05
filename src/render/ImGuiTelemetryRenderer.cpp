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
#include <string>
#include <string_view>
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
                ImGuiTableFlags_SizingFixedFit)) {
        return;
    }

    ImGui::TableSetupColumn(
        "Метрика",
        ImGuiTableColumnFlags_WidthFixed,
        176.0F);
    ImGui::TableSetupColumn(
        "Значение",
        ImGuiTableColumnFlags_WidthStretch);

    statusRowColored(
        "RTSP",
        counters.rtspConnected ? kSignalGreen : kErrorRed,
        counters.rtspConnected ? kIconCheckCircle : kIconWarning,
        counters.rtspConnected ? "ПОДКЛЮЧЕН" : "ОТКЛЮЧЕН");
    statusRowColored(
        "Видео",
        counters.videoSignalPresent ? kSignalGreen : kWarningAmber,
        counters.videoSignalPresent ? kIconCheckCircle : kIconVideoOff,
        counters.videoSignalPresent ? "СИГНАЛ" : "НЕТ СИГНАЛА");
    statusRowColored(
        "CVM1 / Jetson",
        counters.cvSignalPresent ? kSignalGreen : kWarningAmber,
        counters.cvSignalPresent ? kIconCheckCircle : kIconVideoOff,
        counters.cvSignalPresent ? "СИГНАЛ" : "НЕТ СИГНАЛА");

    char buffer[128]{};
    if (counters.decodedFrames == 0U) {
        statusRow("Возраст кадра", "нет данных");
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu мс",
            static_cast<unsigned long long>(counters.videoFrameAgeMs));
        statusRow("Возраст кадра", buffer);
    }

    if (counters.metadataPackets == 0U) {
        statusRow("Возраст CVM1", "нет данных");
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu мс",
            static_cast<unsigned long long>(counters.cvPacketAgeMs));
        statusRow("Возраст CVM1", buffer);
    }

    const auto addU64 = [&](const char* label, std::uint64_t value) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(value));
        statusRow(label, buffer);
    };

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu / %llu",
        static_cast<unsigned long long>(counters.decoderSessions),
        static_cast<unsigned long long>(counters.reconnects));
    statusRow("Сессии / реконнекты", buffer);

    addU64("Декодировано", counters.decodedFrames);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu / %llu",
        static_cast<unsigned long long>(counters.rawPresentedFrames),
        static_cast<unsigned long long>(counters.overlayPresentedFrames));
    statusRow("Raw / Overlay", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu / %llu",
        static_cast<unsigned long long>(counters.overlayMissingMetadataDrops),
        static_cast<unsigned long long>(counters.metadataSequenceGaps));
    statusRow("Exact / потери UDP", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%zu / %zu",
        counters.overlayBufferDepth,
        counters.metadataStoreDepth);
    statusRow("Overlay / буфер CV", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%zu / %llu / %llu",
        counters.recorderQueueDepth,
        static_cast<unsigned long long>(counters.recorderQueueDrops),
        static_cast<unsigned long long>(counters.recorderFailures));
    statusRow("Запись q/drop/fail", buffer);

    ImGui::EndTable();
}

void drawPerformance(const RateHistory& history) {
    ImGui::TextDisabled(
        "Окно 60 с, исходные измерения (без сглаживания / интерполяции)");

    drawRateStatsLine(
        "Декодер",
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
        ImGui::TextDisabled("Сбор данных производительности...");
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
        "секунд назад",
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
        "Декодер",
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
            ImGui::Text("t = %.2f с", history.secondsAgo[index]);
            ImGui::Separator();
            ImGui::TextColored(
                kSignalGreen,
                "Декодер  %.2f FPS",
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

void drawStatusAndPerformance(
    const telemetry::Counters& counters,
    const RateHistory& history) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float statusWidth = std::clamp(
        availableWidth * 0.34F,
        330.0F,
        420.0F);

    if (!ImGui::BeginTable(
            "status_performance",
            2,
            ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn(
        "Статус",
        ImGuiTableColumnFlags_WidthFixed,
        statusWidth);
    ImGui::TableSetupColumn(
        "Производительность",
        ImGuiTableColumnFlags_WidthStretch);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SeparatorText("СТАТУС");
    drawStatus(counters);

    ImGui::TableNextColumn();
    ImGui::SeparatorText("ПРОИЗВОДИТЕЛЬНОСТЬ");
    drawPerformance(history);

    ImGui::EndTable();
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
        "Цели: %zu   DETR: %zu   OFA: %zu",
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

    ImGui::TableSetupColumn("Цель");
    ImGui::TableHeadersRow();
    for (const auto& target : targets) {
        const bool propagated = (target.flags & kPropagatedFlag) != 0U;
        const char* source = propagated
            ? "OFA"
            : ((target.flags & kDetectorFlag) != 0U ? "DETR" : "CV");

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

std::string localizeEventMessage(std::string_view message) {
    if (message == "Application started") {
        return "Приложение запущено";
    }
    if (message == "RTSP session opened") {
        return "RTSP-сессия открыта";
    }
    if (message == "RTSP decoder session ended unexpectedly") {
        return "RTSP-сессия декодера неожиданно завершена";
    }
    if (message == "RTSP session failed: unknown error") {
        return "Ошибка RTSP-сессии: неизвестная ошибка";
    }
    if (message.starts_with("RTSP session failed: ")) {
        constexpr std::string_view prefix = "RTSP session failed: ";
        return std::string("Ошибка RTSP-сессии: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("RTSP reconnect attempt ")) {
        constexpr std::string_view prefix = "RTSP reconnect attempt ";
        std::string_view rest = message.substr(prefix.size());
        const std::size_t after = rest.find(" after ");
        if (after != std::string_view::npos) {
            const std::string_view attempt = rest.substr(0, after);
            std::string_view delay = rest.substr(after + 7U);
            if (delay.ends_with(" ms")) {
                delay.remove_suffix(3U);
                return std::string("Попытка переподключения RTSP ") +
                    std::string(attempt) + " через " +
                    std::string(delay) + " мс";
            }
        }
        return std::string("Попытка переподключения RTSP ") +
            std::string(rest);
    }
    if (message == "Metadata receiver failed") {
        return "Ошибка приёмника метаданных";
    }
    if (message.starts_with("Metadata receiver failed: ")) {
        constexpr std::string_view prefix = "Metadata receiver failed: ";
        return std::string("Ошибка приёмника метаданных: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("Raw screenshot requested: ")) {
        constexpr std::string_view prefix = "Raw screenshot requested: ";
        return std::string("Запрошен снимок Raw: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message ==
        "Raw screenshot request ignored because one is already pending") {
        return "Запрос снимка Raw пропущен: предыдущий ещё обрабатывается";
    }
    if (message.starts_with("Overlay screenshot requested: ")) {
        constexpr std::string_view prefix = "Overlay screenshot requested: ";
        return std::string("Запрошен снимок Overlay: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message ==
        "Overlay screenshot request ignored because one is already pending") {
        return "Запрос снимка Overlay пропущен: предыдущий ещё обрабатывается";
    }
    if (message ==
        "Overlay screenshot requested while Overlay output is disabled") {
        return "Запрошен снимок Overlay при отключённом выводе Overlay";
    }
    if (message == "Display topology changed") {
        return "Топология дисплеев изменилась";
    }
    if (message == "Raw Vulkan surface recovered") {
        return "Поверхность Vulkan Raw восстановлена";
    }
    if (message == "Overlay Vulkan surface recovered") {
        return "Поверхность Vulkan Overlay восстановлена";
    }
    if (message == "Telemetry Vulkan surface recovered") {
        return "Поверхность Vulkan телеметрии восстановлена";
    }
    if (message == "Retiring Vulkan video session resources") {
        return "Освобождение ресурсов видеосессии Vulkan";
    }
    if (message == "Video signal restored") {
        return "Видеосигнал восстановлен";
    }
    if (message == "NO SIGNAL: decoded video is not arriving") {
        return "НЕТ СИГНАЛА: декодированное видео не поступает";
    }
    if (message == "CVM1 / Jetson signal restored") {
        return "Сигнал CVM1 / Jetson восстановлен";
    }
    if (message == "NO SIGNAL: CVM1 metadata is not arriving") {
        return "НЕТ СИГНАЛА: метаданные CVM1 не поступают";
    }
    if (message.starts_with("Raw surface lost: ")) {
        constexpr std::string_view prefix = "Raw surface lost: ";
        return std::string("Потеряна поверхность Raw: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("Overlay surface lost: ")) {
        constexpr std::string_view prefix = "Overlay surface lost: ";
        return std::string("Потеряна поверхность Overlay: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("Telemetry surface lost: ")) {
        constexpr std::string_view prefix = "Telemetry surface lost: ";
        return std::string("Потеряна поверхность телеметрии: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("Blackbox recorder degraded: ")) {
        constexpr std::string_view prefix = "Blackbox recorder degraded: ";
        return std::string("Деградация Blackbox recorder: ") +
            std::string(message.substr(prefix.size()));
    }
    return std::string(message);
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

        const std::string message = localizeEventMessage(event.message);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::Text(
            "-%7.2fс  %-5s  %s",
            ageSeconds,
            telemetry::toString(event.severity),
            message.c_str());
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
        if (!ImGui::Begin("Reg Телеметрия", nullptr, flags)) {
            ImGui::End();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, kAccentBlue);
        ImGui::TextUnformatted("REG / ТЕЛЕМЕТРИЯ / BLACKBOX");
        ImGui::PopStyleColor();
        ImGui::Separator();
        drawStatusAndPerformance(snapshot.counters, rateHistory);
        ImGui::SeparatorText("ТЕКУЩИЕ ЦЕЛИ");
        drawTargets(snapshot.targets);
        ImGui::SeparatorText("СОБЫТИЯ / ПОСЛЕДНИЕ 5 МИНУТ");
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