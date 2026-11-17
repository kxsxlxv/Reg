#include "render/ImGuiTelemetryRenderer.hpp"
#include "render/GtRangeDashboard.hpp"
#include "render/GtDepthScale.hpp"

#include "app/RuntimeUiState.hpp"
#include "render/ImGuiTheme.hpp"
#include "render/VideoOverlayRecorder.hpp"
#include "platform/SdlImGuiKeyMap.hpp"
#include "vulkan/BlankRenderer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanError.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
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

constexpr double kPerformanceWindowSeconds = 60.0;

const ImVec4 kAccentBlue{0.290F, 0.565F, 0.851F, 1.000F};
const ImVec4 kSignalGreen{0.25F, 1.0F, 0.25F, 1.0F};
const ImVec4 kWarningAmber{1.0F, 0.72F, 0.12F, 1.0F};
const ImVec4 kErrorRed{1.0F, 0.35F, 0.32F, 1.0F};
const ImVec4 kRtspAccent{0.66F, 0.57F, 0.95F, 1.0F};

// Compact GT cards follow the same auto-height, subtly tinted child-card
// pattern as Launcher. Every visible control has a tooltip instead of a
// permanent explanatory paragraph.
bool beginGtCard(const char* id,const ImVec4& tint,const ImVec4& accent) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,tint);
    ImGui::PushStyleColor(
        ImGuiCol_Border,ImVec4(accent.x,accent.y,accent.z,0.35F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10.0F,9.0F));
    const bool visible=ImGui::BeginChild(
        id,ImVec2(0.0F,119.0F),
        ImGuiChildFlags_Borders|
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return visible;
}

void gtTooltip(const char* text) {
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s",text);
}

void gtOpacityControl(const char* id,std::atomic<float>& setting,float rightReserve) {
    const float available=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0F,available-rightReserve));
    int percent=static_cast<int>(std::lround(
        std::clamp(setting.load(std::memory_order_relaxed),0.0F,1.0F)*100.0F));
    if(ImGui::SliderInt(id,&percent,0,100,"%d%%"))
        setting.store(static_cast<float>(percent)/100.0F,
                      std::memory_order_relaxed);
    gtTooltip("Прозрачность наложения на видео.\n"
              "0% — слой скрыт.\n100% — непрозрачный слой.");
}

void gtSmoothingControl(const char* id,std::atomic_bool& setting,
                        const char* description) {
    bool enabled=setting.load(std::memory_order_relaxed);
    if(ImGui::Checkbox(id,&enabled))
        setting.store(enabled,std::memory_order_relaxed);
    gtTooltip(description);
}

// Show true KSM1 tint as a translucent overlay against a small checkerboard.
void drawGtSkyLegend(float opacity) {
    const float width=std::max(32.0F,ImGui::GetContentRegionAvail().x);
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    constexpr float height=16.0F;
    ImGui::InvisibleButton("##gt-sky-legend",ImVec2(width,height));
    auto* draw=ImGui::GetWindowDrawList();
    const float cell=width/12.0F;
    for(int i=0;i<12;++i)
        draw->AddRectFilled(
            ImVec2(origin.x+cell*static_cast<float>(i),origin.y),
            ImVec2(origin.x+cell*static_cast<float>(i+1),origin.y+height),
            IM_COL32((i&1)?47:31,(i&1)?56:40,(i&1)?67:51,255));
    draw->AddRectFilled(origin,ImVec2(origin.x+width,origin.y+height),
        ImGui::GetColorU32(ImVec4(0.15F,0.93F,0.97F,
                                std::clamp(opacity,0.0F,1.0F))));
    draw->AddRect(origin,ImVec2(origin.x+width,origin.y+height),
                  IM_COL32(77,206,211,130));
    gtTooltip("Бирюзовый — небо; неизвестные пиксели прозрачны. KSM1 растягивается на Raw RGB.");
}

// The three palette stops MUST match depthColor() in shaders/gt_map.frag.
// Logarithmic normalisation puts the geometric distance midpoint at 50%.
void drawGtDepthLegend(float nearM,float farM,float width) {
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    constexpr float height=16.0F;
    ImGui::InvisibleButton("##gt-depth-legend",ImVec2(width,height));
    auto* draw=ImGui::GetWindowDrawList();
    const float middle=origin.x+width*0.5F;
    draw->AddRectFilledMultiColor(origin,ImVec2(middle,origin.y+height),
        IM_COL32(250,77,41,255),IM_COL32(245,214,46,255),
        IM_COL32(245,214,46,255),IM_COL32(250,77,41,255));
    draw->AddRectFilledMultiColor(ImVec2(middle,origin.y),
        ImVec2(origin.x+width,origin.y+height),
        IM_COL32(245,214,46,255),IM_COL32(31,125,245,255),
        IM_COL32(31,125,245,255),IM_COL32(245,214,46,255));
    draw->AddRect(origin,ImVec2(origin.x+width,origin.y+height),
                  IM_COL32(240,240,240,85));
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Логарифмическая глубина Z, м.\n"
                         "Ближняя граница: %.1f м\n"
                         "Середина: %.1f м\n"
                         "Дальняя граница: %.1f м",
                         nearM,gtDepthLogMidpoint(nearM,farM),farM);
}

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
    const RateHistory& history,
    float panelHeight) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float healthWidth = std::clamp(
        availableWidth * 0.34F,
        320.0F,
        380.0F);

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
        Key,
        Text,
    };

    struct PendingInputEvent {
        PendingInputKind kind{};
        float x{};
        float y{};
        int button{-1};
        bool value{};
        ImGuiKey key{ImGuiKey_None};
        SDL_Keymod mods{};
        std::string text;
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
    bool textInputActive{false};
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
    GtRangeDashboard gtDashboard;
    bool rangeWasPaused{false};
    std::chrono::steady_clock::time_point pausedRangeAt{};
    std::shared_ptr<const std::vector<telemetry::GtRangeTrack>> pausedRangeTracks;
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
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event->key.windowID == self->windowId) {
                input.kind = PendingInputKind::Key;
                input.key = platform::sdlKeyToImGuiKey(
                    event->key.key,event->key.scancode);
                input.mods = event->key.mod;
                input.value = event->type == SDL_EVENT_KEY_DOWN;
                relevant = true;
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (event->text.windowID == self->windowId) {
                // SDL text buffer is only guaranteed valid for the callback.
                // Do not pass raw pointers across the event queue.
                input.kind = PendingInputKind::Text;
                input.text = event->text.text != nullptr
                    ? event->text.text : "";
                relevant = !input.text.empty();
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
            case PendingInputKind::Key:
                io.AddKeyEvent(ImGuiMod_Ctrl,
                    (event.mods & SDL_KMOD_CTRL) != 0);
                io.AddKeyEvent(ImGuiMod_Shift,
                    (event.mods & SDL_KMOD_SHIFT) != 0);
                io.AddKeyEvent(ImGuiMod_Alt,
                    (event.mods & SDL_KMOD_ALT) != 0);
                io.AddKeyEvent(ImGuiMod_Super,
                    (event.mods & SDL_KMOD_GUI) != 0);
                if (event.key != ImGuiKey_None)
                    io.AddKeyEvent(event.key,event.value);
                break;
            case PendingInputKind::Text:
                io.AddInputCharactersUTF8(event.text.c_str());
                break;
            }
        }
    }

    void synchronizeTextInput() {
        // SDL3 IME/text input is scoped to its window. Raw remains the
        // keyboard target when Raw is focused, including NetImgui/U.
        const bool desired =
            ImGui::GetIO().WantTextInput &&
            SDL_GetKeyboardFocus() == window;
        if (desired && !textInputActive) {
            if (SDL_StartTextInput(window)) {
                textInputActive = true;
            } else {
                std::cerr << "[telemetry] SDL_StartTextInput failed: "
                          << SDL_GetError() << '\n';
            }
        } else if (!desired && textInputActive) {
            static_cast<void>(SDL_StopTextInput(window));
            textInputActive = false;
        }
    }

    void stopTextInput() noexcept {
        if (textInputActive && window != nullptr) {
            static_cast<void>(SDL_StopTextInput(window));
            textInputActive = false;
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

    static void drawGtControls() {
        if(!ImGui::BeginTable("##gt-three-cards",3,ImGuiTableFlags_SizingStretchSame))
            return;
        const ImVec4 bbox{0.40F,0.80F,0.97F,1.0F};
        const ImVec4 sky{0.24F,0.91F,0.85F,1.0F};
        const ImVec4 depth{0.99F,0.68F,0.31F,1.0F};
        ImGui::TableNextColumn();
        if(beginGtCard("##gt-card-bbox",ImVec4(0.095F,0.133F,0.185F,1.0F),bbox)) {
            bool enabled=app::showGtBoxes();
            ImGui::PushStyleColor(ImGuiCol_Text,bbox);
            const bool boxesChanged=ImGui::Checkbox("Рамки##gt-boxes",&enabled);
            ImGui::PopStyleColor();
            if(boxesChanged)
                app::setShowGtBoxes(enabled);
            gtTooltip("Рамки объектов на Raw.\nГорячая клавиша T.");
            bool ids=app::showGtIds();
            if(ImGui::Checkbox("ID##gt-ids",&ids))
                app::setShowGtIds(ids);
            gtTooltip("Идентификаторы объектов возле рамок.");
            bool ranges=app::showGtRanges();
            if(ImGui::Checkbox("Дальность##gt-ranges",&ranges))
                app::setShowGtRanges(ranges);
            gtTooltip("Расстояние до корня объекта (range_m).\n"
                      "Не путать с оптической глубиной Z.");
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if(beginGtCard("##gt-card-sky",ImVec4(0.085F,0.155F,0.165F,1.0F),sky)) {
            bool enabled=app::showGtSky();
            ImGui::PushStyleColor(ImGuiCol_Text,sky);
            const bool skyChanged=ImGui::Checkbox("Небо##gt-sky",&enabled);
            ImGui::PopStyleColor();
            if(skyChanged)
                app::gShowGtSky.store(enabled,std::memory_order_relaxed);
            gtTooltip("KSM1: маска неба на RGB.");
            const float right=ImGui::GetFrameHeight()+ImGui::GetStyle().ItemSpacing.x;
            gtOpacityControl("##gt-sky-opacity",app::gGtSkyOpacity,right);
            ImGui::SameLine();
            gtSmoothingControl("##gt-sky-smooth",app::gGtSkySmooth,
                "Сглаживание маски неба.\n"
                "Выключено: Nearest, чёткие границы.\n"
                "Включено: Linear, плавные края.");
            drawGtSkyLegend(app::gGtSkyOpacity.load(std::memory_order_relaxed));
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if(beginGtCard("##gt-card-depth",ImVec4(0.170F,0.124F,0.124F,1.0F),depth)) {
            bool enabled=app::showGtDepth();
            ImGui::PushStyleColor(ImGuiCol_Text,depth);
            const bool depthChanged=ImGui::Checkbox("Карта глубины##gt-depth",&enabled);
            ImGui::PopStyleColor();
            if(depthChanged)
                app::gShowGtDepth.store(enabled,std::memory_order_relaxed);
            gtTooltip("KDM1: оптическая глубина Z, м.\n"
                      "Логарифмическая цветовая шкала.");
            const float right=ImGui::GetFrameHeight()+ImGui::GetStyle().ItemSpacing.x;
            gtOpacityControl("##gt-depth-opacity",app::gGtDepthOpacity,right);
            ImGui::SameLine();
            gtSmoothingControl("##gt-depth-smooth",app::gGtDepthSmooth,
                "Сглаживание глубины.\n"
                "Linear усредняет только валидные Z.\n"
                "Нулевые значения остаются прозрачными.");
            float nearM=std::max(0.1F,app::gGtDepthNearM.load(std::memory_order_relaxed));
            float farM=std::max(nearM+0.1F,app::gGtDepthFarM.load(std::memory_order_relaxed));
            const float available=ImGui::GetContentRegionAvail().x;
            const float rangeWidth=std::min(135.0F,std::max(82.0F,available*0.43F));
            drawGtDepthLegend(nearM,farM,
                std::max(32.0F,available-rangeWidth-ImGui::GetStyle().ItemSpacing.x));
            ImGui::SameLine();
            if(ImGui::SmallButton("Диапазон##gt-z-range"))
                ImGui::OpenPopup("##gt-depth-range-popup");
            gtTooltip("Изменить ближнюю и дальнюю границы Z, м.");
            if(ImGui::BeginPopup("##gt-depth-range-popup")) {
                ImGui::TextUnformatted("Диапазон Z, м");
                ImGui::SetNextItemWidth(130.0F);
                if(ImGui::InputFloat("От##gt-near",&nearM,0.0F,0.0F,"%.1f")) {
                    nearM=std::clamp(nearM,0.1F,farM-0.1F);
                    app::gGtDepthNearM.store(nearM,std::memory_order_relaxed);
                }
                gtTooltip("Ближняя граница логарифмической шкалы, м.");
                ImGui::SetNextItemWidth(130.0F);
                if(ImGui::InputFloat("До##gt-far",&farM,0.0F,0.0F,"%.1f")) {
                    farM=std::max(farM,nearM+0.1F);
                    app::gGtDepthFarM.store(farM,std::memory_order_relaxed);
                }
                gtTooltip("Дальний предел Z, м.\n"
                          "Увеличьте его для аэросъёмки.");
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }

    static void drawGtDebug(const telemetry::Snapshot& snapshot) {
        const auto& counters=snapshot.counters;
        const auto& diagnostics=snapshot.gtCvDiagnostics;
        if(!ImGui::BeginTable("##gt-debug-columns",3,
            ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV))
            return;
        ImGui::TableNextColumn();
        ImGui::SeparatorText("CVM1 / FrameKey");
        ImGui::Text("UDP / decoded: %llu / %llu",
            static_cast<unsigned long long>(counters.metadataPackets),
            static_cast<unsigned long long>(counters.metadataValidFrames));
        ImGui::Text("Accepted / targets: %llu / %llu",
            static_cast<unsigned long long>(counters.metadataAcceptedFrames),
            static_cast<unsigned long long>(counters.metadataTargets));
        ImGui::Text("Joined / matched CV: %llu / %llu",
            static_cast<unsigned long long>(diagnostics.joinedFrames),
            static_cast<unsigned long long>(diagnostics.matchedTargets));
        ImGui::Text("RGF1 missing: %llu",
            static_cast<unsigned long long>(counters.identityMissingFrames));
        ImGui::Text("GT unavailable: %llu",
            static_cast<unsigned long long>(diagnostics.gtUnavailableFrames));
        ImGui::Text("CV unmatched / expired / pending: %llu / %llu / %llu",
            static_cast<unsigned long long>(diagnostics.unmatchedCvFrames),
            static_cast<unsigned long long>(diagnostics.expiredCvFrames),
            static_cast<unsigned long long>(diagnostics.pendingCvFrames));
        if(counters.metadataPackets>0 && counters.metadataValidFrames==0)
            ImGui::TextColored(kErrorRed,"CVM1 decoder rejected datagrams");
        else if(counters.metadataAcceptedFrames>0 && diagnostics.joinedFrames==0)
            ImGui::TextColored(kWarningAmber,"No exact GT FrameKey joins");
        else if(diagnostics.joinedFrames>0 && diagnostics.matchedTargets==0 &&
                counters.metadataTargets>0)
            ImGui::TextColored(kWarningAmber,"No CV matches above IoU 0.30");
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Карты / GPU");
        const auto rawW=app::gPresentedRawWidth.load(std::memory_order_relaxed);
        const auto rawH=app::gPresentedRawHeight.load(std::memory_order_relaxed);
        ImGui::Text("Небо: %ux%u / Raw %ux%u",
            app::gPresentedSkyWidth.load(),app::gPresentedSkyHeight.load(),rawW,rawH);
        ImGui::Text("Глубина: %ux%u / Raw %ux%u",
            app::gPresentedDepthWidth.load(),app::gPresentedDepthHeight.load(),rawW,rawH);
        ImGui::Text("GPU Небо / Глубина: %s / %s",
            app::gPresentedSkyMapReady.load()?"ready":"absent",
            app::gPresentedDepthMapReady.load()?"ready":"absent");
        ImGui::Text("Worker Небо / Глубина: %llu / %llu",
            static_cast<unsigned long long>(counters.gtWorkerSkyReady),
            static_cast<unsigned long long>(counters.gtWorkerDepthReady));
        ImGui::Text("Queue: %zu / %zu KiB",
            counters.gtWorkerQueueFrames,counters.gtWorkerQueueBytes/1024U);
        ImGui::Text("Queue drops / lock drops: %llu / %llu",
            static_cast<unsigned long long>(counters.gtWorkerQueueDrops),
            static_cast<unsigned long long>(counters.gtWorkerLockDrops));
        ImGui::Text("CRC/FNV errors: %llu",
            static_cast<unsigned long long>(counters.gtWorkerChecksumFailures));
        if(app::gPresentedGtGpuError.load(std::memory_order_relaxed))
            ImGui::TextColored(kErrorRed,"Vulkan compositor error");
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Кадры / IMU");
        ImGui::Text("GT status: %s",
            media::gtStatusName(app::gPresentedGtStatus.load(std::memory_order_relaxed)));
        ImGui::Text("Ready / not ready: %llu / %llu",
            static_cast<unsigned long long>(app::gGtReadyFrames.load()),
            static_cast<unsigned long long>(app::gGtNotReadyFrames.load()));
        ImGui::Text("Missing / mismatch / parse: %llu / %llu / %llu",
            static_cast<unsigned long long>(app::gGtMissingFrames.load()),
            static_cast<unsigned long long>(app::gGtMismatchedFrames.load()),
            static_cast<unsigned long long>(app::gGtParseErrors.load()));
        ImGui::Text("Per-AU ready / deferred / timeout: %llu / %llu / %llu",
            static_cast<unsigned long long>(counters.gtFrameAttachedReady),
            static_cast<unsigned long long>(counters.gtFrameDeferred),
            static_cast<unsigned long long>(counters.gtFrameDeferredTimeout));
        ImGui::Text("KGT1 / IMU ready: %llu / %llu",
            static_cast<unsigned long long>(counters.gtKgt1Frames),
            static_cast<unsigned long long>(counters.imuReadyFrames));
        ImGui::Text("IMU invalid / null: %llu / %llu",
            static_cast<unsigned long long>(counters.imuInvalidFrames),
            static_cast<unsigned long long>(counters.imuNullFrames));
        ImGui::Text("Parse max / JSON max: %llu us / %llu bytes",
            static_cast<unsigned long long>(counters.gtParseMaxUs),
            static_cast<unsigned long long>(counters.gtMaxJsonBytes));
        ImGui::EndTable();
        if(ImGui::TreeNode("Дополнительные счётчики##gt-extra")) {
            const auto rawW=app::gPresentedRawWidth.load(std::memory_order_relaxed);
            const auto rawH=app::gPresentedRawHeight.load(std::memory_order_relaxed);
            ImGui::Text("KSM1: %ux%u -> Raw %ux%u",
                app::gPresentedSkyWidth.load(),app::gPresentedSkyHeight.load(),rawW,rawH);
            ImGui::Text("KDM1: %ux%u -> Raw %ux%u",
                app::gPresentedDepthWidth.load(),app::gPresentedDepthHeight.load(),rawW,rawH);
            const auto skyStatus=app::gPresentedSkyStatus.load(std::memory_order_relaxed);
            const auto depthStatus=app::gPresentedDepthStatus.load(std::memory_order_relaxed);
            const auto mapName=[](media::GtMapStatus s) {
                switch(s) {
                    case media::GtMapStatus::Missing:return "missing";
                    case media::GtMapStatus::NotReady:return "not_ready";
                    case media::GtMapStatus::AmbiguousViewport:return "ambiguous_viewport";
                    case media::GtMapStatus::Unavailable:return "unavailable";
                    case media::GtMapStatus::Encoded:return "encoded (worker asynchronous)";
                    case media::GtMapStatus::Ready:return "ready";
                    case media::GtMapStatus::Invalid:return "invalid";
                    case media::GtMapStatus::FrameMismatch:return "frame_mismatch";
                }
                return "unknown";
            };
            ImGui::Text("Небо: %s | GPU map: %s",mapName(skyStatus),
                        app::gPresentedSkyMapReady.load()?"ready":"absent");
            ImGui::Text("Глубина: %s | GPU map: %s",mapName(depthStatus),
                        app::gPresentedDepthMapReady.load()?"ready":"absent");
            if (app::gPresentedGtGpuError.load(std::memory_order_relaxed))
                ImGui::TextColored(kErrorRed,
                    "Vulkan GT compositor disabled: see Reg stderr for error");
            ImGui::SeparatorText("GT / KSM1-KDM1 WORKER");
            ImGui::Text("Небо декодировано / ошибочно: %llu / %llu",
                static_cast<unsigned long long>(counters.gtWorkerSkyReady),
                static_cast<unsigned long long>(counters.gtWorkerInvalidSky));
            ImGui::Text("Глубина декодирована / ошибочно: %llu / %llu",
                static_cast<unsigned long long>(counters.gtWorkerDepthReady),
                static_cast<unsigned long long>(counters.gtWorkerInvalidDepth));
            ImGui::Text("Ошибок CRC/FNV: %llu | queue drops: %llu | lock drops: %llu",
                static_cast<unsigned long long>(counters.gtWorkerChecksumFailures),
                static_cast<unsigned long long>(counters.gtWorkerQueueDrops),
                static_cast<unsigned long long>(counters.gtWorkerLockDrops));
            ImGui::Text("Очередь: %zu кадр. / %zu KiB | возраст: %llu ms",
                counters.gtWorkerQueueFrames,
                counters.gtWorkerQueueBytes/1024U,
                static_cast<unsigned long long>(counters.gtWorkerQueueAgeMs));
            ImGui::Text("Декодирование суммарно / max: %llu / %llu us",
                static_cast<unsigned long long>(counters.gtWorkerTotalUs),
                static_cast<unsigned long long>(counters.gtWorkerMaxUs));
            ImGui::Text("Готовый кэш: %zu кадр. / %zu MiB (макс. 128 / 96 MiB)",
                counters.gtWorkerCacheFrames,counters.gtWorkerCacheBytes/(1024U*1024U));
            ImGui::Text("GT per-AU (показ): ready %llu | отсрочки %llu | timeout %llu",
                static_cast<unsigned long long>(counters.gtFrameAttachedReady),
                static_cast<unsigned long long>(counters.gtFrameDeferred),
                static_cast<unsigned long long>(counters.gtFrameDeferredTimeout));
            ImGui::Text("Fallback GT cache lookup: hit %llu | miss %llu | busy %llu",
                static_cast<unsigned long long>(counters.gtWorkerLookupHits),
                static_cast<unsigned long long>(counters.gtWorkerLookupMisses),
                static_cast<unsigned long long>(counters.gtWorkerLookupBusy));
            ImGui::Text("Вытеснено: по возрасту %llu | по лимиту %llu",
                static_cast<unsigned long long>(counters.gtWorkerEvictedAge),
                static_cast<unsigned long long>(counters.gtWorkerEvictedCapacity));
            const auto mapCount=std::min(counters.gtWorkerSkyReady,counters.gtWorkerDepthReady);
            if(mapCount>counters.decodedFrames)
                ImGui::Text("GT ahead of RGB decode: %llu frames",
                    static_cast<unsigned long long>(mapCount-counters.decodedFrames));
            ImGui::SeparatorText("GT / ТЕКУЩИЙ ОТОБРАЖЁННЫЙ КАДР");
            const auto status = app::gPresentedGtStatus.load(
                std::memory_order_relaxed);
            ImGui::Text("Статус: %s", media::gtStatusName(status));
            ImGui::SeparatorText("GT / ДЕКОДИРОВАННЫЕ КАДРЫ");
            ImGui::Text("ready: %llu",
                static_cast<unsigned long long>(app::gGtReadyFrames.load()));
            ImGui::Text("not_ready / busy / unavailable: %llu",
                static_cast<unsigned long long>(app::gGtNotReadyFrames.load()));
            ImGui::Text("missing: %llu",
                static_cast<unsigned long long>(app::gGtMissingFrames.load()));
            ImGui::Text("parse_error: %llu",
                static_cast<unsigned long long>(app::gGtParseErrors.load()));
            ImGui::Text("frame_mismatch: %llu",
                static_cast<unsigned long long>(app::gGtMismatchedFrames.load()));
            ImGui::SeparatorText("KGT1 / CAMERA-IMU (c начала процесса)");
            ImGui::Text("KGT1: %llu | size >256KiB: %llu",
                        static_cast<unsigned long long>(counters.gtKgt1Frames),
                        static_cast<unsigned long long>(counters.gtOversizePackets));
            ImGui::Text("IMU ready / invalid / null: %llu / %llu / %llu",
                        static_cast<unsigned long long>(counters.imuReadyFrames),
                        static_cast<unsigned long long>(counters.imuInvalidFrames),
                        static_cast<unsigned long long>(counters.imuNullFrames));
            ImGui::Text("IMU samples (overlapping windows): %llu",
                        static_cast<unsigned long long>(counters.imuSamplesParsed));
            ImGui::Text("KGT1 parse total / max: %llu / %llu us | JSON max: %llu bytes",
                        static_cast<unsigned long long>(counters.gtParseTotalUs),
                        static_cast<unsigned long long>(counters.gtParseMaxUs),
                        static_cast<unsigned long long>(counters.gtMaxJsonBytes));
            ImGui::Spacing();
    
            ImGui::TreePop();
        }
    }

    void drawOverview(const telemetry::Snapshot& snapshot,
                      std::chrono::steady_clock::time_point now) {
        const float displayHeight=ImGui::GetContentRegionAvail().y;
        drawPerformanceAndHealth(snapshot.counters,rateHistory,
            std::clamp(displayHeight*0.35F,160.0F,320.0F));
        ImGui::SeparatorText("GT");
        if(ImGui::CollapsingHeader("Настройки##gt-settings",
                ImGuiTreeNodeFlags_DefaultOpen)) {
            drawGtControls();
        }
        // The timeline always uses remaining space, independently of settings.
        // No scrolling in Overview; object cards use pagination.
        const bool paused=app::rangeTimelinePaused();
        if(paused && !rangeWasPaused) {
            pausedRangeAt=now;
            pausedRangeTracks=snapshot.gtRangeTracks;
        }
        if(!paused && rangeWasPaused) pausedRangeTracks.reset();
        rangeWasPaused=paused;
        const auto& view=paused?pausedRangeTracks:snapshot.gtRangeTracks;
        static const std::vector<telemetry::GtRangeTrack> empty;
        static bool debugOpen=false;
        const float available=ImGui::GetContentRegionAvail().y;
        const float debugReserve=debugOpen?235.0F:32.0F;
        const float graphHeight=std::max(85.0F,available-debugReserve);
        if(ImGui::BeginChild("##overview-gt-charts",
                ImVec2(0.0F,graphHeight),ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
            gtDashboard.draw(view?*view:empty,snapshot.counters,
                snapshot.gtCvDiagnostics,paused?pausedRangeAt:now,paused);
        }
        ImGui::EndChild();
        ImGui::SetNextItemOpen(debugOpen,ImGuiCond_Always);
        if(ImGui::CollapsingHeader("DEBUG##overview-gt-debug")) {
            debugOpen=true;
            drawGtDebug(snapshot);
        } else debugOpen=false;
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
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("Reg Телеметрия", nullptr, flags)) {
            ImGui::End();
            return;
        }

        if (ImGui::BeginTabBar("telemetry_tabs")) {
            const app::TelemetryPage selectedPage =
                app::telemetryPage();
            const ImGuiTabItemFlags overviewFlags =
                (selectedPage == app::TelemetryPage::Overview || selectedPage == app::TelemetryPage::Gt)
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
            const ImGuiTabItemFlags eventsFlags =
                selectedPage == app::TelemetryPage::Events
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
            const bool eventsOpen = ImGui::BeginTabItem(
                "События", nullptr, eventsFlags);
            if (ImGui::IsItemClicked()) {
                app::setTelemetryPage(app::TelemetryPage::Events);
            }
            if (eventsOpen) {
                drawEvents(snapshot.events, now);
                ImGui::EndTabItem();
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
        synchronizeTextInput();
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
    impl_->stopTextInput();
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
