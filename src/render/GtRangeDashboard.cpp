#include "render/GtRangeDashboard.hpp"
#include "render/Cvm1SemanticStyle.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reg::render {
namespace {
using Clock = std::chrono::steady_clock;

constexpr ImVec4 kDefaultLine{0.26F, 0.79F, 1.0F, 1.0F};
constexpr ImVec4 kDefaultDetection{1.0F, 0.72F, 0.28F, 1.0F};
constexpr ImVec4 kDefaultTracking{0.49F, 0.97F, 0.62F, 1.0F};
constexpr ImVec4 kDefaultOther{0.69F, 0.59F, 1.0F, 1.0F};

double secondsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(a - b).count();
}

ImVec4 parseHex(std::string_view hex, ImVec4 fallback) {
    if (hex.size() != 7 || hex[0] != '#')
        return fallback;
    auto digit = [](char ch) {
        if (ch >= '0' && ch <= '9') return static_cast<int>(ch - '0');
        if (ch >= 'A' && ch <= 'F') return static_cast<int>(ch - 'A') + 10;
        if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch - 'a') + 10;
        return -1;
    };
    unsigned rgb[3]{};
    for (int i = 0; i < 3; ++i) {
        const int hi = digit(hex[1 + i * 2]);
        const int lo = digit(hex[2 + i * 2]);
        if (hi < 0 || lo < 0) return fallback;
        rgb[i] = static_cast<unsigned>(hi * 16 + lo);
    }
    return ImVec4(rgb[0] / 255.0F, rgb[1] / 255.0F,
                  rgb[2] / 255.0F, 1.0F);
}

struct ChartSettings {
    double windowSeconds{60.0};
    double gapSeconds{0.4};
    ImVec4 rangeColor{kDefaultLine};
};

std::filesystem::path settingsPath() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec && std::filesystem::exists(cwd / "config/gt-range-ui.json", ec))
        return cwd / "config/gt-range-ui.json";
    if (const char* base = SDL_GetBasePath(); base != nullptr)
        return std::filesystem::path(base) / "config/gt-range-ui.json";
    return "config/gt-range-ui.json";
}

bool visible(double x, const ChartSettings& settings) {
    return x >= -settings.windowSeconds && x <= 0.0;
}

} // namespace

struct GtRangeDashboard::Impl {
    ChartSettings settings;
    std::filesystem::path path;
    std::filesystem::file_time_type lastModified{};
    bool loadedOnce{false};
    Clock::time_point nextCheck{};
    std::string focusedId;
    int page{};
    std::size_t markersDrawnLastFrame{};

    void reloadConfig(Clock::time_point now) {
        if (now < nextCheck) return;
        nextCheck = now + std::chrono::milliseconds(300);
        const auto resolved = settingsPath();
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(resolved, ec);
        if (ec) return; // Sensible defaults when the optional file is absent.
        if (loadedOnce && resolved == path && mtime == lastModified)
            return;

        loadedOnce = true;
        path = resolved;
        lastModified = mtime;
        try {
            std::ifstream stream(path);
            const auto json = nlohmann::json::parse(stream);
            ChartSettings next;
            next.windowSeconds = std::clamp(
                json.value("window_seconds", next.windowSeconds), 5.0, 300.0);
            next.gapSeconds = std::clamp(
                json.value("gap_ms", next.gapSeconds * 1000.0) / 1000.0,
                0.05, 5.0);
            next.rangeColor = parseHex(
                json.value("range_color", std::string("#43C9FF")),
                kDefaultLine);
            settings = std::move(next);
        } catch (const std::exception& e) {
            // Never crash the renderer or discard the last valid settings.
            std::cerr << "[gt-chart] invalid config " << path.string()
                      << ": " << e.what() << '\n';
        }
    }

    void plotCard(const telemetry::GtRangeTrack& track,
                  float height,
                  Clock::time_point anchor,
                  bool paused) {
        ImGui::PushID(track.id.c_str());
        constexpr ImGuiWindowFlags noScroll =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::BeginChild("gt_card", ImVec2(-1.0F, height),
                              ImGuiChildFlags_Borders, noScroll)) {
            ImGui::TextColored(settings.rangeColor,
                "%s", track.id.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("  %.2f м", track.latestMeters);
            ImGui::SameLine();
            ImGui::TextDisabled("  CV: %zu", track.matches.size());
            if (!focusedId.empty() && focusedId == track.id) {
                ImGui::SameLine();
                ImGui::TextDisabled("  [нажмите для возврата]");
            }

            const float graphHeight =
                std::max(60.0F, ImGui::GetContentRegionAvail().y);
            if (ImPlot::BeginPlot("##gt_range", ImVec2(-1.0F, graphHeight),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxis(ImAxis_X1, "сек", ImPlotAxisFlags_NoMenus);
                ImPlot::SetupAxisLimits(
                    ImAxis_X1, -settings.windowSeconds, 0.0,
                    paused ? ImPlotCond_Once : ImPlotCond_Always);
                ImPlot::SetupAxis(ImAxis_Y1, "м",
                    ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit |
                    ImPlotAxisFlags_NoMenus);

                ImPlotSpec lineSpec;
                lineSpec.LineColor = settings.rangeColor;
                lineSpec.LineWeight = 2.0F;
                lineSpec.Flags = ImPlotItemFlags_NoLegend;
                std::vector<double> xs, ys;
                xs.reserve(track.points.size());
                ys.reserve(track.points.size());
                std::size_t segment = 0;
                auto flush = [&]() {
                    const std::string name =
                        "##distance_" + std::to_string(segment++);
                    if (xs.size() > 1) {
                        ImPlot::PlotLine(name.c_str(),
                            xs.data(), ys.data(),
                            static_cast<int>(xs.size()), lineSpec);
                    } else if (xs.size() == 1) {
                        ImPlotSpec single = lineSpec;
                        single.Marker = ImPlotMarker_Circle;
                        single.MarkerSize = 3.0F;
                        ImPlot::PlotScatter(name.c_str(), xs.data(),
                            ys.data(), 1, single);
                    }
                    xs.clear();
                    ys.clear();
                };
                Clock::time_point previous{};
                for (const auto& sample : track.points) {
                    const double t = secondsBetween(sample.time, anchor);
                    if (!visible(t, settings)) {
                        if (!xs.empty()) flush();
                        continue;
                    }
                    if (!xs.empty() &&
                        secondsBetween(sample.time, previous) >
                        settings.gapSeconds) flush();
                    xs.push_back(t);
                    ys.push_back(sample.meters);
                    previous = sample.time;
                }
                flush();

                const auto palette = cvm1::current();
                for (std::size_t group = 0;
                     group < cvm1::kStyleCount; ++group) {
                    std::vector<double> mx, my;
                    for (const auto& m : track.matches) {
                        const double x = secondsBetween(m.time, anchor);
                        if (!visible(x, settings) ||
                            cvm1::Palette::index(palette->resolve(m.flags)) != group ||
                            !palette->style(m.flags).visible)
                            continue;
                        mx.push_back(x);
                        my.push_back(m.meters);
                    }
                    if (mx.empty()) continue;
                    markersDrawnLastFrame += mx.size();
                    const auto& style = palette->styles[group];
                    const ImVec4 color(style.color.r, style.color.g,
                                       style.color.b, style.color.a);
                    ImPlotSpec marker;
                    marker.LineColor = color;
                    marker.MarkerLineColor = color;
                    marker.MarkerFillColor = color;
                    marker.Marker = style.pattern == cvm1::Pattern::dashed
                        ? ImPlotMarker_Cross : (style.pattern == cvm1::Pattern::corners
                        ? ImPlotMarker_Square : ImPlotMarker_Diamond);
                    marker.MarkerSize = style.markerSize;
                    marker.Flags = ImPlotItemFlags_NoLegend;
                    const std::string label =
                        "##cv_group_" + std::to_string(group);
                    ImPlot::PlotScatter(label.c_str(),
                        mx.data(), my.data(), static_cast<int>(mx.size()), marker);
                }

                if (ImPlot::IsPlotHovered()) {
                    const ImVec2 mouse = ImGui::GetMousePos();
                    double nearest = std::numeric_limits<double>::infinity();
                    const telemetry::GtRangePoint* nearestPoint = nullptr;
                    const telemetry::GtCvMatch* nearestHit = nullptr;
                    // Pixel-space hit testing avoids reporting a value far
                    // from the pointer on a sparse curve.
                    for (const auto& sample : track.points) {
                        const double t = secondsBetween(sample.time, anchor);
                        if (!visible(t, settings)) continue;
                        const ImVec2 pix = ImPlot::PlotToPixels(t, sample.meters);
                        const double dx = pix.x - mouse.x, dy = pix.y - mouse.y;
                        const double d = dx * dx + dy * dy;
                        if (d < nearest) {
                            nearest = d;
                            nearestPoint = &sample;
                            nearestHit = nullptr;
                        }
                    }
                    for (const auto& hit : track.matches) {
                        const double t = secondsBetween(hit.time, anchor);
                        if (!visible(t, settings)) continue;
                        const ImVec2 pix = ImPlot::PlotToPixels(t, hit.meters);
                        const double dx = pix.x - mouse.x, dy = pix.y - mouse.y;
                        const double d = dx * dx + dy * dy;
                        if (d < nearest) {
                            nearest = d;
                            nearestPoint = nullptr;
                            nearestHit = &hit;
                        }
                    }
                    if (nearest < 196.0) { // 14 px cursor tolerance
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(track.id.c_str());
                        if (nearestHit) {
                            const auto& style = cvm1::current()->style(nearestHit->flags);
                            ImGui::TextColored(ImVec4(style.color.r, style.color.g,
                                style.color.b, style.color.a), "%s", style.label.c_str());
                            ImGui::Text("GT: %.2f м (измерено)", nearestHit->meters);
                            if (cvm1::isEphemeralId(nearestHit->trackId))
                                ImGui::Text("CV frame-local ID  class: %u",
                                    static_cast<unsigned>(nearestHit->classId));
                            else
                                ImGui::Text("CV ID: %llu  class: %u",
                                    static_cast<unsigned long long>(nearestHit->trackId),
                                    static_cast<unsigned>(nearestHit->classId));
                            ImGui::Text("IoU: %.3f   confidence: %.3f",
                                nearestHit->iou, static_cast<double>(nearestHit->confidence));
                            ImGui::Text("Frame: %llu/%llu",
                                static_cast<unsigned long long>(nearestHit->frame.streamEpoch),
                                static_cast<unsigned long long>(nearestHit->frame.frameId));
                            ImGui::Text("source_time_ns: %llu",
                                static_cast<unsigned long long>(nearestHit->sourceTimeNs));
                        } else if (nearestPoint) {
                            ImGui::Text("GT: %.2f м (измерено)", nearestPoint->meters);
                            ImGui::Text("Frame: %llu/%llu",
                                static_cast<unsigned long long>(nearestPoint->frame.streamEpoch),
                                static_cast<unsigned long long>(nearestPoint->frame.frameId));
                            ImGui::Text("source_time_ns: %llu",
                                static_cast<unsigned long long>(nearestPoint->sourceTimeNs));
                        }
                        ImGui::EndTooltip();
                    }
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        focusedId = focusedId == track.id ? "" : track.id;
                        page = 0;
                    }
                }
                ImPlot::EndPlot();
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    void draw(const std::vector<telemetry::GtRangeTrack>& tracks,
              const telemetry::Counters& counters,
              const telemetry::GtCvDiagnostics& diagnostics,
              Clock::time_point anchor,
              bool paused) {
        reloadConfig(Clock::now());
        if (paused)
            ImGui::TextColored(kDefaultDetection, "ПАУЗА");
        else
            ImGui::TextDisabled("GT range  /  CVM1 IoU (FrameKey)");
        ImGui::TextDisabled("CVM1 UDP %llu  decoded %llu  accepted %llu  targets %llu",
            static_cast<unsigned long long>(counters.metadataPackets),
            static_cast<unsigned long long>(counters.metadataValidFrames),
            static_cast<unsigned long long>(counters.metadataAcceptedFrames),
            static_cast<unsigned long long>(counters.metadataTargets));
        ImGui::TextDisabled("Exact FrameKey joins %llu  matched CV %llu  plotted %zu",
            static_cast<unsigned long long>(diagnostics.joinedFrames),
            static_cast<unsigned long long>(diagnostics.matchedTargets),
            markersDrawnLastFrame);
        ImGui::TextDisabled("RGF1 missing %llu  GT unavailable %llu  unmatched CV %llu  expired CV %llu  pending CV %llu",
            static_cast<unsigned long long>(counters.identityMissingFrames),
            static_cast<unsigned long long>(diagnostics.gtUnavailableFrames),
            static_cast<unsigned long long>(diagnostics.unmatchedCvFrames),
            static_cast<unsigned long long>(diagnostics.expiredCvFrames),
            static_cast<unsigned long long>(diagnostics.pendingCvFrames));
        if (counters.metadataPackets > 0 && counters.metadataValidFrames == 0)
            ImGui::TextColored(ImVec4(1.0F, 0.30F, 0.30F, 1.0F),
                "CVM1 received but protocol decoder rejected every datagram");
        else if (counters.metadataAcceptedFrames > 0 && diagnostics.joinedFrames == 0)
            ImGui::TextColored(ImVec4(1.0F, 0.58F, 0.22F, 1.0F),
                "CVM1 accepted, but there are no exact GT FrameKey joins");
        else if (diagnostics.joinedFrames > 0 && diagnostics.matchedTargets == 0 && counters.metadataTargets > 0)
            ImGui::TextColored(ImVec4(1.0F, 0.58F, 0.22F, 1.0F),
                "Exact FrameKey joins exist, but no target passed IoU 0.30");
        ImGui::Separator();
        markersDrawnLastFrame = 0;

        if (tracks.empty()) {
            ImGui::TextDisabled("Ожидание GT с полем range_m...");
            return;
        }
        if (!focusedId.empty() &&
            std::none_of(tracks.begin(), tracks.end(), [&](const auto& t) {
                return t.id == focusedId;
            })) focusedId.clear();

        // A fixed-size viewport, one column, no vertical scrollbars. Paging is
        // necessary when the number of tracks exceeds physical monitor height.
        const float fullHeight = std::max(70.0F, ImGui::GetContentRegionAvail().y);
        const int capacity = focusedId.empty()
            ? std::max(1, static_cast<int>((fullHeight - 28.0F) / 145.0F))
            : 1;
        const int total = static_cast<int>(tracks.size());
        const int pages = std::max(1, (total + capacity - 1) / capacity);
        page = std::clamp(page, 0, pages - 1);

        if (focusedId.empty() && pages > 1) {
            if (ImGui::SmallButton("<") && page > 0) --page;
            ImGui::SameLine();
            ImGui::Text("%d / %d  (%d объектов)", page + 1, pages, total);
            ImGui::SameLine();
            if (ImGui::SmallButton(">") && page + 1 < pages) ++page;
        }

        constexpr ImGuiWindowFlags noScroll =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::BeginChild("gt_range_cards", ImVec2(-1.0F, -1.0F),
                              ImGuiChildFlags_None, noScroll)) {
            const auto avail = ImGui::GetContentRegionAvail();
            const int first = page * capacity;
            const int end = std::min(total, first + capacity);
            const int count = focusedId.empty() ? end - first : 1;
            const float spacing = ImGui::GetStyle().ItemSpacing.y;
            const float itemHeight = std::max(
                80.0F, (avail.y - spacing * (count - 1)) / count);
            if (!focusedId.empty()) {
                for (const auto& track : tracks) {
                    if (track.id == focusedId) {
                        plotCard(track, itemHeight, anchor, paused);
                        break;
                    }
                }
            } else {
                for (int i = first; i < end; ++i)
                    plotCard(tracks[static_cast<std::size_t>(i)],
                             itemHeight, anchor, paused);
            }
        }
        ImGui::EndChild();
    }
};

GtRangeDashboard::GtRangeDashboard() : impl_(std::make_unique<Impl>()) {}
GtRangeDashboard::~GtRangeDashboard() = default;

void GtRangeDashboard::draw(
    const std::vector<telemetry::GtRangeTrack>& tracks,
    const telemetry::Counters& counters,
    const telemetry::GtCvDiagnostics& diagnostics,
    Clock::time_point anchor,
    bool paused) {
    impl_->draw(tracks, counters, diagnostics, anchor, paused);
}

} // namespace reg::render
