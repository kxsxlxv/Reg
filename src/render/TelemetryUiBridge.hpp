#pragma once

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace ImGui {
namespace reg_telemetry_bridge {

struct State {
    bool compactTargets{};
    int setupColumn{};
    int field{};
    ImGuiContext* imGuiOwner{};
    ImPlotContext* imPlotContext{};
};

inline State& state() noexcept {
    static thread_local State value;
    return value;
}

inline void ensureImPlotContext() {
    auto& value = state();
    ImGuiContext* current = ImGui::GetCurrentContext();
    if (value.imPlotContext == nullptr || value.imGuiOwner != current) {
        value.imPlotContext = ImPlot::CreateContext();
        value.imGuiOwner = current;
    }
    ImPlot::SetCurrentContext(value.imPlotContext);
}

} // namespace reg_telemetry_bridge

inline void PlotLinesReg(
    const char* label,
    const float* values,
    int valuesCount,
    int valuesOffset = 0,
    const char* overlayText = nullptr,
    float scaleMin = FLT_MAX,
    float scaleMax = FLT_MAX,
    ImVec2 graphSize = ImVec2(0, 0),
    int stride = sizeof(float)) {
    if (values == nullptr || valuesCount <= 0) {
        return;
    }

    reg_telemetry_bridge::ensureImPlotContext();

    const ImVec2 size{
        graphSize.x == 0.0F ? -1.0F : graphSize.x,
        graphSize.y > 0.0F ? graphSize.y : 120.0F,
    };

    if (overlayText != nullptr && *overlayText != '\0') {
        ImGui::TextUnformatted(overlayText);
    }

    if (!ImPlot::BeginPlot(label, size, ImPlotFlags_NoLegend)) {
        return;
    }

    ImPlot::SetupAxis(
        ImAxis_X1,
        nullptr,
        ImPlotAxisFlags_NoLabel |
            ImPlotAxisFlags_NoTickLabels);

    ImPlotAxisFlags yFlags = ImPlotAxisFlags_NoLabel;
    if (scaleMin == FLT_MAX || scaleMax == FLT_MAX) {
        yFlags |= ImPlotAxisFlags_AutoFit;
    }
    ImPlot::SetupAxis(
        ImAxis_Y1,
        "FPS",
        yFlags);

    const int start = std::clamp(valuesOffset, 0, valuesCount - 1);
    const int count = valuesCount - start;

    if (count > 1) {
        ImPlot::SetupAxisLimits(
            ImAxis_X1,
            0.0,
            static_cast<double>(count - 1),
            ImPlotCond_Always);
    }

    if (scaleMin != FLT_MAX && scaleMax != FLT_MAX) {
        ImPlot::SetupAxisLimits(
            ImAxis_Y1,
            static_cast<double>(scaleMin),
            static_cast<double>(scaleMax),
            ImPlotCond_Always);
    }

    if (stride == static_cast<int>(sizeof(float))) {
        ImPlot::PlotLine("##fps", values + start, count);
    } else {
        struct StridedData {
            const unsigned char* bytes{};
            int stride{};
        } data{
            reinterpret_cast<const unsigned char*>(values) +
                static_cast<std::ptrdiff_t>(start) * stride,
            stride,
        };

        ImPlot::PlotLineG(
            "##fps",
            [](int index, void* raw) {
                const auto* strided = static_cast<const StridedData*>(raw);
                float value = 0.0F;
                std::memcpy(
                    &value,
                    strided->bytes +
                        static_cast<std::ptrdiff_t>(index) * strided->stride,
                    sizeof(value));
                return ImPlotPoint{
                    static_cast<double>(index),
                    static_cast<double>(value),
                };
            },
            &data,
            count);
    }

    ImPlot::EndPlot();
}

inline bool BeginTableReg(
    const char* strId,
    int columns,
    ImGuiTableFlags flags = 0,
    const ImVec2& outerSize = ImVec2(0.0F, 0.0F),
    float innerWidth = 0.0F) {
    auto& state = reg_telemetry_bridge::state();
    state.compactTargets =
        strId != nullptr && std::strcmp(strId, "targets") == 0;
    state.setupColumn = 0;
    state.field = 0;

    int actualColumns = state.compactTargets ? 1 : columns;
    if (strId != nullptr &&
        std::strcmp(strId, "status") == 0 &&
        ImGui::GetIO().DisplaySize.y > ImGui::GetIO().DisplaySize.x) {
        actualColumns = 2;
    }

    return ImGui::BeginTable(
        strId,
        actualColumns,
        flags,
        outerSize,
        innerWidth);
}

inline void TableSetupColumnReg(
    const char* label,
    ImGuiTableColumnFlags flags = 0,
    float initWidthOrWeight = 0.0F,
    ImGuiID userId = 0) {
    auto& state = reg_telemetry_bridge::state();
    if (!state.compactTargets) {
        ImGui::TableSetupColumn(
            label,
            flags,
            initWidthOrWeight,
            userId);
        return;
    }

    if (state.setupColumn++ == 0) {
        ImGui::TableSetupColumn(
            "Target / bbox",
            flags,
            initWidthOrWeight,
            userId);
    }
}

inline void TableHeadersRowReg() {
    ImGui::TableHeadersRow();
}

inline void TableNextRowReg(
    ImGuiTableRowFlags rowFlags = 0,
    float minRowHeight = 0.0F) {
    auto& state = reg_telemetry_bridge::state();
    state.field = 0;
    ImGui::TableNextRow(rowFlags, minRowHeight);
}

inline bool TableNextColumnReg() {
    auto& state = reg_telemetry_bridge::state();
    if (!state.compactTargets) {
        return ImGui::TableNextColumn();
    }

    static constexpr const char* labels[]{
        "ID", "C", "conf", "x", "y", "w", "h",
    };

    const int field = state.field++;
    bool visible = true;
    if (field == 0) {
        visible = ImGui::TableNextColumn();
    } else {
        ImGui::SameLine(0.0F, 10.0F);
    }

    if (field >= 0 && field < static_cast<int>(std::size(labels))) {
        ImGui::TextDisabled("%s", labels[field]);
        ImGui::SameLine(0.0F, 3.0F);
    }

    return visible;
}

inline void EndTableReg() {
    ImGui::EndTable();
    auto& state = reg_telemetry_bridge::state();
    state.compactTargets = false;
    state.setupColumn = 0;
    state.field = 0;
}

} // namespace ImGui

#define PlotLines PlotLinesReg
#define BeginTable BeginTableReg
#define TableSetupColumn TableSetupColumnReg
#define TableHeadersRow TableHeadersRowReg
#define TableNextRow TableNextRowReg
#define TableNextColumn TableNextColumnReg
#define EndTable EndTableReg
