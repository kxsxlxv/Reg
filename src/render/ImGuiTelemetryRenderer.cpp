#include "render/ImGuiTelemetryRenderer.hpp"

#include "app/RuntimeUiState.hpp"
#include "render/ImGuiTheme.hpp"
#include "render/VideoOverlayRecorder.hpp"
#include "vulkan/BlankRenderer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
const ImVec4 kRtspAccent{0.66F, 0.57F, 0.95F, 1.0F};

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

void drawRateValue(
    const RateStats& stats,
    double value,
    const ImVec4& color,
    bool prominent) {
    ImGui::TableNextColumn();
    if (!stats.valid) {
        ImGui::TextDisabled("—");
        return;
    }

    if (prominent) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::Text("%.2f", value);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%.2f", value);
    }
}

void drawRateStatsTable(const RateHistory& history) {
    const RateStats decoded = rateStats(history.decodedFps);
    const RateStats rendered = rateStats(history.rawFps);
    const RateStats cv = rateStats(history.overlayFps);

    if (!ImGui::BeginTable(
            "fps_stats",
            4,
            ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit,
            ImVec2(430.0F, 0.0F))) {
        return;
    }

    ImGui::TableSetupColumn(
        "Метрика",
        ImGuiTableColumnFlags_WidthFixed,
        72.0F);
    ImGui::TableSetupColumn(
        "Декодер",
        ImGuiTableColumnFlags_WidthFixed,
        104.0F);
    ImGui::TableSetupColumn(
        "Рендер",
        ImGuiTableColumnFlags_WidthFixed,
        104.0F);
    ImGui::TableSetupColumn(
        "CV",
        ImGuiTableColumnFlags_WidthFixed,
        88.0F);
    ImGui::TableHeadersRow();

    const auto drawRow = [&](const char* label,
                             double RateStats::*member,
                             bool prominent) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (prominent) {
            ImGui::TextUnformatted(label);
        } else {
            ImGui::TextDisabled("%s", label);
        }
        drawRateValue(decoded, decoded.*member, kSignalGreen, prominent);
        drawRateValue(rendered, rendered.*member, kAccentBlue, prominent);
        drawRateValue(cv, cv.*member, kWarningAmber, prominent);
    };

    drawRow("FPS", &RateStats::current, true);
    drawRow("MIN", &RateStats::minimum, false);
    drawRow("AVG", &RateStats::average, false);
    drawRow("MAX", &RateStats::maximum, false);

    ImGui::EndTable();
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

void drawPipelineStatus(
    bool healthy,
    const char* healthyValue,
    const char* unhealthyValue,
    const ImVec4& unhealthyColor) {
    const ImVec4 color = healthy ? kSignalGreen : unhealthyColor;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text(
        "%s  %s",
        healthy ? kIconCheckCircle : kIconWarning,
        healthy ? healthyValue : unhealthyValue);
    ImGui::PopStyleColor();
}

void drawPipelineSectionHeader(
    const char* label,
    const ImVec4& accent) {
    const ImVec4 separatorColor{
        accent.x,
        accent.y,
        accent.z,
        0.42F};
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::PushStyleColor(ImGuiCol_Separator, separatorColor);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor(2);

    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    if (maximum.y > minimum.y + 4.0F) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(minimum.x, minimum.y + 2.0F),
            ImVec2(minimum.x + 3.0F, maximum.y - 2.0F),
            ImGui::GetColorU32(accent));
    }
}

void drawMetricRow(const char* label, const char* value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    const float width = ImGui::CalcTextSize(value).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    }
    ImGui::TextUnformatted(value);
}

void drawPipelineMetrics(
    const char* tableId,
    std::initializer_list<std::pair<const char*, const char*>> rows) {
    if (!ImGui::BeginTable(
            tableId,
            2,
            ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn(
        "label",
        ImGuiTableColumnFlags_WidthStretch,
        0.65F);
    ImGui::TableSetupColumn(
        "value",
        ImGuiTableColumnFlags_WidthStretch,
        0.35F);
    for (const auto& [label, value] : rows) {
        drawMetricRow(label, value);
    }
    ImGui::EndTable();
}

void drawPipelineHealth(const telemetry::Counters& counters) {
    char frameAge[48]{};
    char cvAge[48]{};
    char sessions[48]{};
    char reconnects[48]{};
    char decoded[48]{};
    char rendered[48]{};
    char cvFrames[48]{};
    char buffers[64]{};
    char exactUdp[64]{};

    if (counters.decodedFrames == 0U) {
        std::snprintf(frameAge, sizeof(frameAge), "—");
    } else {
        std::snprintf(
            frameAge,
            sizeof(frameAge),
            "%llu мс",
            static_cast<unsigned long long>(counters.videoFrameAgeMs));
    }

    if (counters.metadataPackets == 0U) {
        std::snprintf(cvAge, sizeof(cvAge), "—");
    } else {
        std::snprintf(
            cvAge,
            sizeof(cvAge),
            "%llu мс",
            static_cast<unsigned long long>(counters.cvPacketAgeMs));
    }

    std::snprintf(
        sessions,
        sizeof(sessions),
        "%llu",
        static_cast<unsigned long long>(counters.decoderSessions));
    std::snprintf(
        reconnects,
        sizeof(reconnects),
        "%llu",
        static_cast<unsigned long long>(counters.reconnects));
    std::snprintf(
        decoded,
        sizeof(decoded),
        "%llu",
        static_cast<unsigned long long>(counters.decodedFrames));
    std::snprintf(
        rendered,
        sizeof(rendered),
        "%llu",
        static_cast<unsigned long long>(counters.rawPresentedFrames));
    std::snprintf(
        cvFrames,
        sizeof(cvFrames),
        "%llu",
        static_cast<unsigned long long>(counters.overlayPresentedFrames));
    std::snprintf(
        buffers,
        sizeof(buffers),
        "%zu / %zu",
        counters.overlayBufferDepth,
        counters.metadataStoreDepth);
    std::snprintf(
        exactUdp,
        sizeof(exactUdp),
        "%llu / %llu",
        static_cast<unsigned long long>(counters.overlayMissingMetadataDrops),
        static_cast<unsigned long long>(counters.metadataSequenceGaps));

    drawPipelineSectionHeader("RTSP", kRtspAccent);
    drawPipelineStatus(
        counters.rtspConnected,
        "ПОДКЛЮЧЕН",
        "ОТКЛЮЧЕН",
        kErrorRed);
    drawPipelineMetrics("rtsp_metrics", {
        {"Сессии", sessions},
        {"Реконнекты", reconnects},
    });

    drawPipelineSectionHeader("ВИДЕО", kAccentBlue);
    drawPipelineStatus(
        counters.videoSignalPresent,
        "СИГНАЛ",
        "НЕТ СИГНАЛА",
        kWarningAmber);
    drawPipelineMetrics("video_metrics", {
        {"Кадр", frameAge},
        {"Декод", decoded},
        {"Рендер", rendered},
    });

    drawPipelineSectionHeader("CV / JETSON", kWarningAmber);
    drawPipelineStatus(
        counters.cvSignalPresent,
        "СИГНАЛ",
        "НЕТ СИГНАЛА",
        kWarningAmber);
    drawPipelineMetrics("cv_metrics", {
        {"Пакет", cvAge},
        {"Кадры", cvFrames},
        {"Буфер", buffers},
        {"Exact / UDP", exactUdp},
    });

    if (counters.recorderQueueDrops > 0U || counters.recorderFailures > 0U) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningAmber);
        ImGui::Text(
            "%s  RECORDER: drop %llu / fail %llu",
            kIconWarning,
            static_cast<unsigned long long>(counters.recorderQueueDrops),
            static_cast<unsigned long long>(counters.recorderFailures));
        ImGui::PopStyleColor();
    }
}

void drawPerformanceGraph(
    const RateHistory& history,
    float height) {
    if (history.secondsAgo.empty()) {
        ImGui::TextDisabled("Сбор данных производительности...");
        return;
    }

    if (!ImPlot::BeginPlot(
            "##fps",
            ImVec2(-1.0F, std::max(height, 150.0F)),
            ImPlotFlags_NoTitle)) {
        return;
    }

    ImPlot::SetupAxis(
        ImAxis_X1,
        "секунд",
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
        "Рендер",
        history.secondsAgo.data(),
        history.rawFps.data(),
        count,
        rawSpec);

    ImPlotSpec overlaySpec;
    overlaySpec.LineColor = kWarningAmber;
    overlaySpec.LineWeight = 2.0F;
    ImPlot::PlotLine(
        "CV",
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
                "Рендер   %.2f FPS",
                history.rawFps[index]);
            ImGui::TextColored(
                kWarningAmber,
                "CV       %.2f FPS",
                history.overlayFps[index]);
            ImGui::EndTooltip();
        }
    }

    ImPlot::EndPlot();
}

void drawPerformanceAndHealth(
    const telemetry::Counters& counters,
    const RateHistory& history) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float healthWidth = std::clamp(
        availableWidth * 0.34F,
        320.0F,
        380.0F);
    const float panelHeight = 390.0F;

    if (!ImGui::BeginTable(
            "performance_health",
            2,
            ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn(
        "Производительность",
        ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(
        "Состояние",
        ImGuiTableColumnFlags_WidthFixed,
        healthWidth);
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    if (ImGui::BeginChild(
            "performance_panel",
            ImVec2(0.0F, panelHeight),
            ImGuiChildFlags_None)) {
        ImGui::SeparatorText("ПРОИЗВОДИТЕЛЬНОСТЬ");
        drawRateStatsTable(history);
        ImGui::Spacing();
        const float graphHeight = ImGui::GetContentRegionAvail().y;
        drawPerformanceGraph(history, graphHeight);
    }
    ImGui::EndChild();

    ImGui::TableNextColumn();
    if (ImGui::BeginChild(
            "pipeline_health",
            ImVec2(0.0F, panelHeight),
            ImGuiChildFlags_None)) {
        drawPipelineHealth(counters);
    }
    ImGui::EndChild();

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
        return std::string("Запрошен снимок Рендер: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message ==
        "Raw screenshot request ignored because one is already pending") {
        return "Запрос снимка Рендер пропущен: предыдущий ещё обрабатывается";
    }
    if (message.starts_with("Overlay screenshot requested: ")) {
        constexpr std::string_view prefix = "Overlay screenshot requested: ";
        return std::string("Запрошен снимок CV: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message ==
        "Overlay screenshot request ignored because one is already pending") {
        return "Запрос снимка CV пропущен: предыдущий ещё обрабатывается";
    }
    if (message ==
        "Overlay screenshot requested while Overlay output is disabled") {
        return "Запрошен снимок CV при отключённом выводе CV";
    }
    if (message == "Display topology changed") {
        return "Топология дисплеев изменилась";
    }
    if (message == "Raw Vulkan surface recovered") {
        return "Поверхность Vulkan Рендер восстановлена";
    }
    if (message == "Overlay Vulkan surface recovered") {
        return "Поверхность Vulkan CV восстановлена";
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
        return std::string("Потеряна поверхность Рендер: ") +
            std::string(message.substr(prefix.size()));
    }
    if (message.starts_with("Overlay surface lost: ")) {
        constexpr std::string_view prefix = "Overlay surface lost: ";
        return std::string("Потеряна поверхность CV: ") +
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


std::string exactKeyLabel(
    const std::optional<media::FrameKey>& key) {
    if (!key) {
        return "—";
    }
    return std::to_string(key->streamEpoch) +
        " / " + std::to_string(key->frameId);
}

void drawExactSyncDiagnostics(const telemetry::Counters& counters) {
    const auto& sync = counters.exactSync;

    if (!ImGui::BeginChild(
            "exact_sync_scroller",
            ImVec2(0.0F, 0.0F),
            false,
            ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    drawPipelineSectionHeader("EXACT SYNC / VIDEO + CVM1", kAccentBlue);
    if (!counters.videoSignalPresent) {
        ImGui::TextColored(kWarningAmber, "ОЖИДАНИЕ ВИДЕО");
    } else if (!counters.cvSignalPresent) {
        ImGui::TextColored(
            kWarningAmber, "ОЖИДАНИЕ CVM1 — детекции не поступают");
    } else if (sync.matchedPairs == 0U) {
        ImGui::TextColored(
            kWarningAmber, "CVM1 есть, точных совпадений пока нет");
    } else {
        ImGui::TextColored(
            kSignalGreen, "Точные пары найдены по epoch + frame_id");
    }
    ImGui::TextDisabled(
        "Последние ID показывают независимые потоки и не обязаны совпадать.");
    ImGui::TextDisabled(
        "Ожидание CVM1 не считается ошибкой идентичности.");

    const auto row = [](const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value.c_str());
    };
    const auto number = [](std::uint64_t value) {
        return std::to_string(value);
    };

    if (ImGui::BeginTable(
            "exact_sync_table",
            2,
            ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(
            "Метрика", ImGuiTableColumnFlags_WidthStretch, 0.58F);
        ImGui::TableSetupColumn(
            "Значение", ImGuiTableColumnFlags_WidthStretch, 0.42F);

        row("Последний video epoch / frame",
            exactKeyLabel(sync.lastVideoKey));
        row("Последний CVM1 epoch / frame",
            exactKeyLabel(sync.lastMetadataKey));
        row("Последняя точная пара",
            exactKeyLabel(sync.lastMatchedKey));
        row("Целей в последней паре",
            number(sync.lastMatchedTargetCount));

        row("Декодировано видео", number(counters.decodedFrames));
        row("Video без SEI",
            number(counters.overlayMissingIdentity));
        row("CVM1 получено", number(counters.metadataPackets));
        row("CVM1 некорректных",
            number(counters.metadataInvalid));
        row("CVM1 дубликатов",
            number(counters.metadataDuplicates));
        row("CVM1 не по порядку",
            number(counters.metadataOutOfOrder));
        row("Пропусков packet sequence",
            number(counters.metadataSequenceGaps));

        row("Точных пар найдено",
            number(sync.matchedPairs));
        row("Overlay показано",
            number(counters.overlayPresentedFrames));
        row("Отвергнуто неверных FrameKey",
            number(sync.rejectedKeyMismatches));
        row("Видео без CVM1 к дедлайну",
            number(sync.dueWithoutMetadata));
        row("Видео вытеснено из буфера",
            number(counters.overlayBufferEvictions));
        row("CVM1 после отброса кадра",
            number(sync.metadataAfterDroppedFrame));
        row("Из них позже дедлайна",
            number(sync.metadataArrivedAfterDeadline));
        row("Поздние CVM1 в точных парах",
            number(sync.matchedAfterDeadline));
        row("CVM1 вытеснено из хранилища",
            number(counters.metadataStoreEvictions));

        row("Буфер видео / вместимость",
            number(counters.overlayBufferDepth) + " / 64");
        row("Хранилище CVM1",
            number(counters.metadataStoreDepth));
        row("Задержка overlay",
            number(counters.configuredOverlayDelayMs) + " ms");

        if (sync.hasTimingSample) {
            char buffer[96]{};
            std::snprintf(
                buffer, sizeof(buffer), "%.2f ms",
                sync.lastArrivalRelativeToDecodeMs);
            row("CVM1 - local video decode", buffer);
            std::snprintf(
                buffer, sizeof(buffer), "%.2f ms",
                sync.lastPlayoutWaitMs);
            row("Decode -> exact pair", buffer);
            std::snprintf(
                buffer, sizeof(buffer),
                "%.2f / %.2f / %.2f ms",
                sync.arrivalDelayP50Ms,
                sync.arrivalDelayP95Ms,
                sync.arrivalDelayP99Ms);
            row("Ожидание CVM1 P50/P95/P99", buffer);
            row("Выборка последних пар",
                number(sync.latencySampleCount));
        } else {
            row("Задержки", "Нет точных пар");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Задержка CVM1 измерена относительно decode в Reg; "
        "отрицательная разница означает CVM1 раньше видео.");
    ImGui::TextDisabled(
        "P50/P95/P99: max(0, receive_time - decode_time), "
        "последние 256 пар.");
    ImGui::TextDisabled(
        "Эти числа НЕ являются измерением Jetson inference "
        "или end-to-end latency.");
    ImGui::EndChild();
}

void drawNetImguiDiagnostics(const NetImguiDiagnostics& diagnostics) {
    ImGui::SeparatorText("NETIMGUI / РЕНДЕР");
    ImGui::TextDisabled(
        "Удалённый Dear ImGui рендерится поверх декодированного видео.");
    ImGui::TextDisabled("F10 — переключить вкладку Telemetry.");
    ImGui::Spacing();

    if (!ImGui::BeginTable(
            "netimgui_diagnostics",
            2,
            ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit)) {
        return;
    }

    ImGui::TableSetupColumn(
        "Метрика",
        ImGuiTableColumnFlags_WidthFixed,
        220.0F);
    ImGui::TableSetupColumn(
        "Значение",
        ImGuiTableColumnFlags_WidthStretch);

    statusRowColored(
        "Server",
        diagnostics.listening ? kSignalGreen : kErrorRed,
        diagnostics.listening ? kIconCheckCircle : kIconWarning,
        diagnostics.listening ? "СЛУШАЕТ" : "НЕ СЛУШАЕТ");
    statusRowColored(
        "Client",
        diagnostics.connected ? kSignalGreen : kWarningAmber,
        diagnostics.connected ? kIconCheckCircle : kIconWarning,
        diagnostics.connected ? "ПОДКЛЮЧЕН" : "ОЖИДАНИЕ");

    char buffer[128]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "TCP %u",
        static_cast<unsigned>(diagnostics.port));
    statusRow("Порт", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u",
        static_cast<unsigned>(diagnostics.connectedClients));
    statusRow("Клиенты", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu B",
        static_cast<unsigned long long>(diagnostics.bytesReceived));
    statusRow("RX", buffer);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu B",
        static_cast<unsigned long long>(diagnostics.bytesSent));
    statusRow("TX", buffer);

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Input forwarding пока отключён: текущий этап проверяет network + draw data + Vulkan overlay.");
}

} // namespace

struct ImGuiTelemetryRenderer::Impl final : VideoOverlayRecorder {
    enum class PendingInputKind : std::uint8_t {
        MousePosition,
        MouseWheel,
        MouseButton,
        MouseLeave,
        Focus,
    };

    struct PendingInputEvent {
        PendingInputKind kind{};
        float x{};
        float y{};
        int button{-1};
        bool value{};
    };

    explicit Impl(
        const vulkan::VulkanContext& context,
        SDL_Window* telemetryWindow)
        : vulkan(context),
          canvas(context),
          window(telemetryWindow),
          windowId(telemetryWindow != nullptr
                       ? SDL_GetWindowID(telemetryWindow)
                       : 0U) {}

    const vulkan::VulkanContext& vulkan;
    vulkan::BlankRenderer canvas;
    SDL_Window* window{nullptr};
    Uint32 windowId{};
    ImGuiContext* context{nullptr};
    ImPlotContext* plotContext{nullptr};
    ImDrawData* drawData{nullptr};
    bool backendInitialized{false};
    bool eventWatchInstalled{false};
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
    std::mutex inputMutex;
    std::vector<PendingInputEvent> pendingInput;

    static int mouseButtonIndex(Uint8 button) noexcept {
        switch (button) {
        case SDL_BUTTON_LEFT:
            return 0;
        case SDL_BUTTON_RIGHT:
            return 1;
        case SDL_BUTTON_MIDDLE:
            return 2;
        case SDL_BUTTON_X1:
            return 3;
        case SDL_BUTTON_X2:
            return 4;
        default:
            return -1;
        }
    }

    static bool SDLCALL eventWatch(
        void* userData,
        SDL_Event* event) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || event == nullptr || self->windowId == 0U) {
            return true;
        }

        PendingInputEvent input{};
        bool relevant = false;

        switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (event->motion.windowID == self->windowId) {
                input.kind = PendingInputKind::MousePosition;
                input.x = event->motion.x;
                input.y = event->motion.y;
                relevant = true;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event->wheel.windowID == self->windowId) {
                input.kind = PendingInputKind::MouseWheel;
                input.x = -event->wheel.x;
                input.y = event->wheel.y;
                relevant = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event->button.windowID == self->windowId) {
                const int button = mouseButtonIndex(event->button.button);
                if (button >= 0) {
                    input.kind = PendingInputKind::MouseButton;
                    input.button = button;
                    input.value = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    relevant = true;
                }
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (event->window.windowID == self->windowId) {
                input.kind = PendingInputKind::MouseLeave;
                relevant = true;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (event->window.windowID == self->windowId) {
                input.kind = PendingInputKind::Focus;
                input.value = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                relevant = true;
            }
            break;
        default:
            break;
        }

        if (relevant) {
            std::scoped_lock lock(self->inputMutex);
            self->pendingInput.push_back(input);
        }

        return true;
    }

    void installEventWatch() {
        if (eventWatchInstalled || window == nullptr) {
            return;
        }
        if (!SDL_AddEventWatch(&Impl::eventWatch, this)) {
            throw std::runtime_error(
                std::string("SDL_AddEventWatch(telemetry) failed: ") +
                SDL_GetError());
        }
        eventWatchInstalled = true;
    }

    void uninstallEventWatch() noexcept {
        if (!eventWatchInstalled) {
            return;
        }
        SDL_RemoveEventWatch(&Impl::eventWatch, this);
        eventWatchInstalled = false;
    }

    void processPendingInput() {
        std::vector<PendingInputEvent> events;
        {
            std::scoped_lock lock(inputMutex);
            events.swap(pendingInput);
        }

        ImGuiIO& io = ImGui::GetIO();
        for (const PendingInputEvent& event : events) {
            switch (event.kind) {
            case PendingInputKind::MousePosition:
                io.AddMousePosEvent(event.x, event.y);
                break;
            case PendingInputKind::MouseWheel:
                io.AddMouseWheelEvent(event.x, event.y);
                break;
            case PendingInputKind::MouseButton:
                io.AddMouseButtonEvent(event.button, event.value);
                break;
            case PendingInputKind::MouseLeave:
                io.AddMousePosEvent(
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max());
                break;
            case PendingInputKind::Focus:
                io.AddFocusEvent(event.value);
                break;
            }
        }
    }

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

    void drawOverview(
        const telemetry::Snapshot& snapshot,
        std::chrono::steady_clock::time_point now) {
        drawPerformanceAndHealth(snapshot.counters, rateHistory);
        ImGui::SeparatorText("ТЕКУЩИЕ ЦЕЛИ");
        drawTargets(snapshot.targets);
        ImGui::SeparatorText("СОБЫТИЯ / ПОСЛЕДНИЕ 5 МИНУТ");
        drawEvents(snapshot.events, now);
    }

    void buildDashboard(
        const telemetry::Snapshot& snapshot,
        const NetImguiDiagnostics& netImgui,
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

        if (ImGui::BeginTabBar("telemetry_tabs")) {
            const app::TelemetryPage selectedPage =
                app::telemetryPage();
            const ImGuiTabItemFlags overviewFlags =
                selectedPage == app::TelemetryPage::Overview
                    ? ImGuiTabItemFlags_SetSelected
                    : ImGuiTabItemFlags_None;
            const ImGuiTabItemFlags exactSyncFlags =
                selectedPage == app::TelemetryPage::ExactSync
                    ? ImGuiTabItemFlags_SetSelected
                    : ImGuiTabItemFlags_None;
            const ImGuiTabItemFlags netImguiFlags =
                selectedPage == app::TelemetryPage::NetImgui
                    ? ImGuiTabItemFlags_SetSelected
                    : ImGuiTabItemFlags_None;

            const bool overviewOpen = ImGui::BeginTabItem(
                "Обзор",
                nullptr,
                overviewFlags);
            if (ImGui::IsItemClicked()) {
                app::setTelemetryPage(app::TelemetryPage::Overview);
            }
            if (overviewOpen) {
                drawOverview(snapshot, now);
                ImGui::EndTabItem();
            }

            const bool exactSyncOpen = ImGui::BeginTabItem(
                "Exact Sync",
                nullptr,
                exactSyncFlags);
            if (ImGui::IsItemClicked()) {
                app::setTelemetryPage(app::TelemetryPage::ExactSync);
            }
            if (exactSyncOpen) {
                drawExactSyncDiagnostics(snapshot.counters);
                ImGui::EndTabItem();
            }

            if (netImgui.enabled) {
                const bool netImguiOpen = ImGui::BeginTabItem(
                    "NetImgui",
                    nullptr,
                    netImguiFlags);
                if (ImGui::IsItemClicked()) {
                    app::setTelemetryPage(app::TelemetryPage::NetImgui);
                }
                if (netImguiOpen) {
                    drawNetImguiDiagnostics(netImgui);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void prepare(
        const telemetry::Snapshot& snapshot,
        const NetImguiDiagnostics& netImgui,
        const vulkan::Swapchain& swapchain) {
        setCurrentContext();
        ensureBackend(swapchain);
        processPendingInput();
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
        buildDashboard(snapshot, netImgui, extent, now);
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
        vulkan::Swapchain& swapchain,
        const NetImguiDiagnostics& netImgui) {
        prepare(snapshot, netImgui, swapchain);
        return canvas.render(swapchain, this);
    }
};

ImGuiTelemetryRenderer::ImGuiTelemetryRenderer(
    const vulkan::VulkanContext& vulkan,
    const vulkan::Swapchain& initialSwapchain)
    : impl_(std::make_unique<Impl>(
          vulkan,
          initialSwapchain.window())) {
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
        impl_->installEventWatch();
    } catch (...) {
        impl_->uninstallEventWatch();
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
    impl_->uninstallEventWatch();
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
    vulkan::Swapchain& swapchain,
    const NetImguiDiagnostics& netImgui) {
    return impl_->render(snapshot, swapchain, netImgui);
}

} // namespace reg::render
