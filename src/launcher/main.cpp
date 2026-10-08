#include "launcher/LauncherConfig.hpp"
#include "launcher/LauncherDisplay.hpp"
#include "launcher/LauncherLayout.hpp"
#include "launcher/MissionPlannerTile.hpp"
#include "launcher/ProcessManager.hpp"
#ifdef _WIN32
#include "launcher/UpdateManager.hpp"
#include <shellapi.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using reg::launcher::Profile;

struct Symbols {
    static constexpr const char* play = "\xee\x80\xb7";
    static constexpr const char* stop = "\xee\x81\x87";
    static constexpr const char* source = "\xee\x81\x8b";
    static constexpr const char* displays = "\xee\x8c\xb3";
    static constexpr const char* record = "\xee\x87\x9b";
    static constexpr const char* network = "\xee\x98\xbe";
    static constexpr const char* tune = "\xee\x90\xa9";
    static constexpr const char* save = "\xee\x85\xa1";
    static constexpr const char* refresh = "\xee\x97\x95";
    static constexpr const char* download = "\xee\x8b\x84";
    static constexpr const char* check = "\xee\xa1\xac";
    static constexpr const char* warning = "\xee\x80\x82";
    static constexpr const char* folder = "\xee\x8b\x87";
    static constexpr const char* logs = "\xee\xa1\xaf";
    static constexpr const char* info = "\xee\xa2\x8e";
    static constexpr const char* add = "\xee\x85\x85";
    static constexpr const char* settings = "\xee\xa2\xb8";
};


template <std::size_t N>
void copyTo(std::array<char, N>& buffer, const std::string& source) {
    std::snprintf(buffer.data(), buffer.size(), "%s", source.c_str());
}

std::string utf8String(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string tail(const fs::path& path, std::streamoff limit = 32768) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto length = input.tellg();
    if (length <= 0) return {};
    const auto begin = std::max<std::streamoff>(0, static_cast<std::streamoff>(length) - limit);
    input.seekg(begin);
    std::string text(static_cast<std::size_t>(static_cast<std::streamoff>(length) - begin), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (begin != 0) {
        const auto newline = text.find('\n');
        if (newline != std::string::npos) text.erase(0, newline + 1);
    }
    return text;
}

int readLauncherDisplay() {
#ifdef _WIN32
    int number = 3; // Default: monitor 3 in Windows Settings > Identify.
#else
    int number = 0;
#endif
    std::ifstream file(reg::launcher::dataDirectory() / "launcher-display.txt");
    int saved = 0;
    if (file >> saved && saved >= 0 && saved <= 16) number = saved;
    return number;
}

void writeLauncherDisplay(int number) {
    const auto path = reg::launcher::dataDirectory() / "launcher-display.txt";
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << number << '\n';
    if (!output) throw std::runtime_error("Не удалось сохранить выбор монитора");
}

bool readMissionPlannerAutoLayout() {
#ifdef _WIN32
    std::ifstream file(reg::launcher::dataDirectory() /
                       "mission-planner-layout.txt");
    int enabled = 0;
    return (file >> enabled) && enabled == 1;
#else
    return false;
#endif
}

void writeMissionPlannerAutoLayout(bool enabled) {
    const auto path = reg::launcher::dataDirectory() /
                      "mission-planner-layout.txt";
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << (enabled ? 1 : 0) << '\n';
    if (!out) throw std::runtime_error("Не удалось сохранить компоновку окон");
}

struct Ui {
    SDL_Window* window{nullptr};
    int launcherDisplay{0};
    std::vector<reg::launcher::LauncherDisplay> availableDisplays;
    std::chrono::steady_clock::time_point lastDisplayScan{};
    Profile profile;
    std::string profileName{"default"};
    std::vector<std::string> profileNames;
    std::array<char, 2048> url{};
    std::array<char, 128> bind{};
    std::array<char, 65> newName{};
    std::string message;
    fs::path sessionDirectory;
    reg::launcher::ProcessManager process;
    std::optional<int> lastExit;
    std::string output;
    std::chrono::steady_clock::time_point lastRead{};
    bool showAdvanced{false};
    bool showLogs{false};
    bool showCreateProfile{false};
#ifdef _WIN32
    bool missionPlannerAutoLayout{readMissionPlannerAutoLayout()};
    bool missionPlannerTiledThisSession{false};
    std::chrono::steady_clock::time_point lastMissionPlannerAttempt{};
#endif
#ifdef _WIN32
    reg::launcher::UpdateManager updater{reg::launcher::executableDirectory()};
    std::string updateChannel{"dev"};
    bool quitForUpdate{false};
    bool autoUpdateStarted{false};
#endif

    explicit Ui(SDL_Window* targetWindow)
        : window(targetWindow), launcherDisplay(readLauncherDisplay()) {
        availableDisplays = reg::launcher::launcherDisplays();
        if (launcherDisplay > 0 && !reg::launcher::moveLauncherToDisplay(
                window, launcherDisplay)) {
            // If the selected display is disconnected, keep the window visible
            // in its default location while retaining the stored preference.
        }
        profileNames = reg::launcher::listProfiles();
        auto selected = reg::launcher::lastSelectedProfile();
        if (std::find(profileNames.begin(), profileNames.end(), selected) == profileNames.end()) {
            selected = profileNames.front();
        }
        selectProfile(selected);
#ifdef _WIN32
        try {
            std::ifstream selectedChannel(reg::launcher::dataDirectory() / "update-channel.txt");
            std::string channel;
            if (std::getline(selectedChannel, channel) && channel == "stable") {
                updateChannel = channel;
            }
        } catch (...) {}
        updater.checkAsync(updateChannel);
#endif
    }

    void selectProfile(const std::string& name) {
        profileName = name;
        profile = reg::launcher::loadProfile(profileName);
        copyTo(url, profile.rtspUrl);
        copyTo(bind, profile.metadataBind);
        reg::launcher::setLastSelectedProfile(name);
        message.clear();
    }

    void commitEdits() {
        profile.rtspUrl = url.data();
        profile.metadataBind = bind.data();
    }

    void save() {
        commitEdits();
        reg::launcher::saveProfile(profileName, profile);
        message = "Профиль сохранён";
    }

    void createProfile() {
        commitEdits();
        const std::string name(newName.data());
        reg::launcher::saveProfile(name, profile);
        profileNames = reg::launcher::listProfiles();
        selectProfile(name);
        newName.fill('\0');
        message = "Новый профиль сохранён";
    }

    void start() {
        commitEdits();
        if (auto error = reg::launcher::validate(profile); !error.empty()) {
            message = error;
            return;
        }
        reg::launcher::saveProfile(profileName, profile);
        sessionDirectory = reg::launcher::newSessionDirectory();
        reg::launcher::saveSessionManifest(sessionDirectory, profileName, profile);
        const auto binary = reg::launcher::executableDirectory() /
#ifdef _WIN32
            "reg_probe.exe";
#else
            "reg_probe";
#endif
        const auto recordings = reg::launcher::dataDirectory() / "recordings" / "blackbox";
        fs::create_directories(recordings);
        process.start(binary, reg::launcher::arguments(profile, recordings), sessionDirectory);
        lastExit.reset();
        lastRead = {};
        output.clear();
        message = "Приложение запущено";
    }

    void update() {
        if (process.running()) {
            if (auto code = process.poll()) {
                lastExit = code;
                message = "Процесс завершён (код " + std::to_string(*code) + ")";
            }
        }
#ifdef _WIN32
        const auto state = updater.snapshot();
        if (updateChannel == "dev" && state.phase == reg::launcher::UpdatePhase::Available &&
            !autoUpdateStarted) {
            autoUpdateStarted = true;
            updater.downloadAsync();
        }
        // Developer channel stages in the background, then installs when the
        // video child is not active. Never interrupt a running RTSP session.
        if (updateChannel == "dev" && state.phase == reg::launcher::UpdatePhase::Prepared &&
            !process.running()) {
            try {
                commitEdits();
                reg::launcher::saveProfile(profileName, profile);
                updater.applyAndRestart();
                quitForUpdate = true;
            } catch (const std::exception& e) {
                message = e.what();
                updateChannel = "stable"; // Avoid endlessly retrying a failed install.
            }
        }
#endif
        const auto now = std::chrono::steady_clock::now();
#ifdef _WIN32
        // Wait for Mission Planner if auto-layout is enabled, then tile once.
        // Do not override the user's later manual window positioning.
        if (missionPlannerAutoLayout && !missionPlannerTiledThisSession &&
            (lastMissionPlannerAttempt == std::chrono::steady_clock::time_point{} ||
             now - lastMissionPlannerAttempt >= std::chrono::seconds(3))) {
            lastMissionPlannerAttempt = now;
            const auto result = reg::launcher::tileWithMissionPlanner(
                window, launcherDisplay);
            if (result == reg::launcher::MissionPlannerTileResult::success) {
                missionPlannerTiledThisSession = true;
                message = "Launcher сверху, Mission Planner снизу";
            }
        }
#endif
        if (lastDisplayScan == std::chrono::steady_clock::time_point{} ||
            now - lastDisplayScan > std::chrono::seconds(3)) {
            availableDisplays = reg::launcher::launcherDisplays();
            lastDisplayScan = now;
        }
        if (!sessionDirectory.empty() &&
            (lastRead == std::chrono::steady_clock::time_point{} ||
             now - lastRead >= std::chrono::milliseconds(500))) {
            // Separate streams retain ordering independently. Both files remain available in full.
            output = "=== STDERR ===\n" + tail(sessionDirectory / "stderr.log") +
                     "\n=== STDOUT ===\n" + tail(sessionDirectory / "stdout.log");
            lastRead = now;
        }
    }


#ifdef _WIN32
    void drawUpdates() {
        using reg::launcher::UpdatePhase;
        const auto info = updater.snapshot();
        if (card("##updates-card", ImVec4(.118f,.143f,.192f,1),
                 Symbols::refresh, "Обновления", ImVec4(.54f,.72f,1,1))) {
            const bool busy = info.phase == UpdatePhase::Checking ||
                              info.phase == UpdatePhase::Downloading;
            ImGui::BeginDisabled(busy);
            ImGui::SetNextItemWidth(148);
            if (ImGui::BeginCombo("##update-channel", updateChannel == "dev"
                                 ? "Development" : "Stable")) {
                for (const auto* option : {"dev", "stable"}) {
                    if (ImGui::Selectable(option, updateChannel == option)) {
                        updateChannel = option;
                        autoUpdateStarted = false;
                        try {
                            fs::create_directories(reg::launcher::dataDirectory());
                            std::ofstream out(reg::launcher::dataDirectory() /
                                              "update-channel.txt", std::ios::trunc);
                            out << updateChannel << '\n';
                        } catch (...) {}
                        updater.checkAsync(updateChannel);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(info.phase == UpdatePhase::Error
                              ? "Повторить" : "Проверить")) {
                autoUpdateStarted = false;
                if (info.phase == UpdatePhase::Error) updater.retryAsync();
                else updater.checkAsync(updateChannel);
            }
            ImGui::EndDisabled();

            if (info.phase == UpdatePhase::Error) {
                ImGui::TextColored(ImVec4(1,.62,.53,1), "%s  Не удалось проверить обновления",
                                   Symbols::warning);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.message.c_str());
            } else {
                ImGui::TextDisabled("%s", info.message.c_str());
            }
            if (info.phase == UpdatePhase::Downloading && info.totalBytes) {
                ImGui::ProgressBar(std::clamp(
                    static_cast<float>(info.downloadedBytes) /
                    static_cast<float>(info.totalBytes), 0.0f, 1.0f),
                    ImVec2(-1,0), "Загрузка");
            }
            if (info.phase == UpdatePhase::Available &&
                updateChannel == "stable") {
                ImGui::SameLine();
                if (ImGui::SmallButton("Скачать")) updater.downloadAsync();
            }
            if (info.phase == UpdatePhase::Prepared && !process.running()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Установить и перезапустить")) {
                    try {
                        commitEdits();
                        reg::launcher::saveProfile(profileName, profile);
                        updater.applyAndRestart();
                        quitForUpdate = true;
                    } catch (const std::exception& e) { message = e.what(); }
                }
            }
        }
        finishCard();
    }
#endif
    // Reusable, consistently padded cards. The panel backgrounds are kept
    // distinct from the window background for readability in the dark theme.
    static bool card(const char* id, ImVec4 background,
                     const char* symbol, const char* label, ImVec4 accent) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.34f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
        // Auto-size vertically to the real number of visible fields. A
        // fixed child height previously clipped the RTSP URL and introduced
        // scrollbars inside almost every card.
        const bool visible = ImGui::BeginChild(
            id, ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (visible) {
            ImGui::TextColored(accent, "%s  %s", symbol, label);
            ImGui::Separator();
        }
        return visible;
    }

    static void finishCard() { ImGui::EndChild(); }

    static void caption(const char* label) {
        ImGui::TextDisabled("%s", label);
    }

    // Put a bounded control on the right instead of occupying the full
    // card width. Inline rows reduce the height of a 1080x1920 portrait UI.
    static void rowControl(const char* label, float width) {
        const float available = ImGui::GetContentRegionAvail().x;
        const float field = std::min(width, available * .48f);
        const float textWidth = ImGui::CalcTextSize(label).x;
        ImGui::AlignTextToFramePadding();
        if (textWidth + field + 16.0f > available) {
            ImGui::TextDisabled("%s", label);
        } else {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
        }
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - field));
        ImGui::SetNextItemWidth(field);
    }

    static void integer(const char* label, const char* id, int& value) {
        rowControl(label, 116.0f);
        // No redundant +/- buttons: type a number or use Ctrl+Click.
        ImGui::InputInt(id, &value, 0, 0);
    }

    void displaySelector(const char* id, const char* label,
                         int& selection, const char* defaultLabel) {
        rowControl(label, 168.0f);
        const std::string preview = selection == 0
            ? std::string("Авто (") + defaultLabel + ")"
            : "Монитор " + std::to_string(selection);
        if (ImGui::BeginCombo(id, preview.c_str())) {
            if (ImGui::Selectable("Автоматически##default", selection == 0))
                selection = 0;
            for (const auto& display : availableDisplays) {
                const std::string item = "Монитор " + std::to_string(display.number);
                if (ImGui::Selectable(item.c_str(), selection == display.number))
                    selection = display.number;
            }
            ImGui::EndCombo();
        }
    }

    void launcherDisplaySelector() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Окно:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(155.0f);
        const std::string label = launcherDisplay == 0
            ? "Системный" : "Монитор " + std::to_string(launcherDisplay);
        if (ImGui::BeginCombo("##launcher-display", label.c_str())) {
            if (ImGui::Selectable("Системный монитор", launcherDisplay == 0)) {
                launcherDisplay = 0;
                writeLauncherDisplay(0);
                reg::launcher::moveLauncherToDisplay(window, 0);
            }
            for (const auto& display : availableDisplays) {
                const std::string option = "Монитор " + std::to_string(display.number);
                if (ImGui::Selectable(option.c_str(), launcherDisplay == display.number)) {
                    launcherDisplay = display.number;
                    writeLauncherDisplay(launcherDisplay);
                    reg::launcher::moveLauncherToDisplay(window, launcherDisplay);
#ifdef _WIN32
                    missionPlannerTiledThisSession = !missionPlannerAutoLayout;
#endif
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Монитор окна лаунчера по нумерации Windows -> Дисплей -> Определить.\nНастройка сохраняется отдельно от профиля.");
        }
    }

    void drawProfileBar() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Профиль:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(196.0f);
        ImGui::BeginDisabled(process.running());
        if (ImGui::BeginCombo("##profile", profileName.c_str())) {
            for (const auto& name : profileNames) {
                if (ImGui::Selectable(name.c_str(), name == profileName)) {
                    try { selectProfile(name); }
                    catch (const std::exception& error) { message = error.what(); }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(process.running());
        if (ImGui::Button("Сохранить", ImVec2(95, 0))) {
            try { save(); } catch (const std::exception& error) {
                message = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Новый", ImVec2(75, 0))) showCreateProfile = !showCreateProfile;
        ImGui::EndDisabled();

        // Keep launcher location independent of the video output mapping.
        // Use the opposite edge of a wide toolbar for the launcher monitor.
        // On smaller windows the selector flows naturally to the next line.
        const float toolbarRight = ImGui::GetWindowContentRegionMax().x;
        const float monitorStart = toolbarRight - 220.0f;
        if (ImGui::GetContentRegionAvail().x > 230.0f &&
            monitorStart > ImGui::GetCursorPosX()) {
            ImGui::SameLine(monitorStart);
        }
        launcherDisplaySelector();
        if (showCreateProfile) {
            ImGui::SetNextItemWidth(210);
            ImGui::InputTextWithHint("##new-name", "Новое имя профиля",
                                     newName.data(), newName.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(process.running());
            if (ImGui::Button("Создать", ImVec2(100, 0))) {
                try { createProfile(); showCreateProfile = false; }
                catch (const std::exception& error) { message = error.what(); }
            }
            ImGui::EndDisabled();
        }
    }

#ifdef _WIN32
    void drawMissionPlannerLayout() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s  Окна:", Symbols::displays);
        ImGui::SameLine();
        if (ImGui::Button("Разместить 50/50 с Mission Planner")) {
            switch (reg::launcher::tileWithMissionPlanner(
                        window, launcherDisplay)) {
                case reg::launcher::MissionPlannerTileResult::success:
                    missionPlannerTiledThisSession = true;
                    message = "Launcher сверху, Mission Planner снизу";
                    break;
                case reg::launcher::MissionPlannerTileResult::notFound:
                    message = "Mission Planner не найден. Запустите его и повторите.";
                    break;
                case reg::launcher::MissionPlannerTileResult::noMonitor:
                    message = "Монитор недоступен или слишком мал для двух окон";
                    break;
                default:
                    message = "Не удалось переместить окна. Проверьте права приложений.";
                    break;
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("При запуске##auto-tiling", &missionPlannerAutoLayout)) {
            try {
                writeMissionPlannerAutoLayout(missionPlannerAutoLayout);
                missionPlannerTiledThisSession = !missionPlannerAutoLayout;
                lastMissionPlannerAttempt = {};
            } catch (const std::exception& e) {
                message = e.what();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Дождаться Mission Planner и один раз разместить "
                "оба окна по половинам выбранного экрана. "
                "Ручные перемещения после этого не переопределяются.");
        }
    }
#endif

    void drawSource() {
        if (card("##source-card", ImVec4(0.105f, 0.151f, 0.195f, 1),
                 Symbols::source, "Источник видео", ImVec4(0.32f, 0.72f, 0.97f, 1))) {
            caption("RTSP / RTSPS адрес");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##rtsp", "rtsp://camera:8554/stream",
                                     url.data(), url.size());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Адрес источника с поддержкой RTSP и RTSPS");
            }
        }
        finishCard();
    }

    void drawDisplays() {
        if (card("##display-card", ImVec4(0.109f, 0.157f, 0.163f, 1),
                 Symbols::displays, "Экраны и наложения", ImVec4(0.34f, 0.84f, 0.71f, 1))) {
            ImGui::Checkbox("CV Overlay", &profile.overlayEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("Телеметрия", &profile.telemetryEnabled);
#ifdef _WIN32
            const char* defaultRaw = "3";
            const char* defaultOverlay = "2";
            const char* defaultTelemetry = "1";
#else
            const char* defaultRaw = "1";
            const char* defaultOverlay = "2";
            const char* defaultTelemetry = "3";
#endif
            displaySelector("##monitor-raw", "Raw / исходное видео",
                            profile.rawDisplay, defaultRaw);
            if (profile.overlayEnabled) {
                displaySelector("##monitor-overlay", "CV Overlay",
                                profile.overlayDisplay, defaultOverlay);
            }
            if (profile.telemetryEnabled) {
                displaySelector("##monitor-telemetry", "Telemetry",
                                profile.telemetryDisplay, defaultTelemetry);
            }
        }
        finishCard();
    }

    void drawRecorder() {
        if (card("##record-card", ImVec4(0.155f, 0.125f, 0.167f, 1),
                 Symbols::record, "Запись Blackbox", ImVec4(0.91f, 0.65f, 0.94f, 1))) {
            ImGui::Checkbox("Включить циклическую запись", &profile.recorderEnabled);
            ImGui::BeginDisabled(!profile.recorderEnabled);
            integer("Длительность сегмента, мс", "##record-segment", profile.recordSegmentMs);
            rowControl("Хранение, мин", 116.0f);
            int minutes = std::max(1, profile.recordRetentionSec / 60);
            if (ImGui::InputInt("##retention-minutes", &minutes, 0, 0)) {
                profile.recordRetentionSec = std::clamp(minutes, 1, 100000) * 60;
            }
            ImGui::TextDisabled("Каталог: recordings/blackbox");
            ImGui::EndDisabled();
        }
        finishCard();
    }

    void drawNetwork() {
        if (card("##network-card", ImVec4(0.124f, 0.143f, 0.197f, 1),
                 Symbols::network, "Сеть и удалённый UI", ImVec4(0.52f, 0.69f, 1, 1))) {
            integer("Порт UDP метаданных", "##metadata-port", profile.metadataPort);
            ImGui::Checkbox("Приём NetImgui", &profile.netImguiEnabled);
            ImGui::BeginDisabled(!profile.netImguiEnabled);
            integer("Порт TCP NetImgui", "##netimgui-port", profile.netImguiPort);
            ImGui::EndDisabled();
        }
        finishCard();
    }

    void drawLatency() {
        if (card("##latency-card", ImVec4(0.176f, 0.140f, 0.114f, 1),
                 Symbols::tune, "Задержка и восстановление", ImVec4(1.0f, 0.73f, 0.40f, 1))) {
            rowControl("Overlay, мс", 116.0f);
            ImGui::SliderInt("##overlay-delay", &profile.overlayDelayMs, 0, 1000);
            integer("Начало переподключения, мс", "##reconnect-initial",
                    profile.reconnectInitialMs);
            integer("Максимум переподключения, мс", "##reconnect-max",
                    profile.reconnectMaxMs);
        }
        finishCard();
    }

    void drawAdvanced() {
        if (!ImGui::CollapsingHeader("Дополнительные параметры",
                                   ImGuiTreeNodeFlags_None)) return;
        ImGui::Indent(8.0f);
        if (ImGui::BeginTable("##advanced-grid", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(.40f,.80f,.97f,1), "%s  Decode / RTSP", Symbols::tune);
            integer("FFmpeg max_delay, мкс", "##max-delay", profile.maxDelayUs);
            integer("RTP reorder queue", "##reorder", profile.reorderQueueSize);
            integer("Дополнительные GPU frames", "##hw-frames", profile.extraHwFrames);
            caption("UDP bind (интерфейс приёма)");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##metadata-bind", bind.data(), bind.size());
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(.90f,.70f,.97f,1), "%s  Диагностика и запись", Symbols::settings);
            integer("Очередь записи", "##record-queue", profile.recordQueueCapacity);
            ImGui::Checkbox("Vulkan validation", &profile.validationEnabled);
            ImGui::Checkbox("Требовать FrameIdentity", &profile.requireFrameIdentity);
            integer("Проверить N кадров (0 = выкл)", "##identity-frames",
                    profile.identityProbeFrames);
            ImGui::EndTable();
        }
        ImGui::Unindent(8.0f);
    }

    void drawLogs() {
        if (!ImGui::CollapsingHeader("Логи текущей сессии",
                                   ImGuiTreeNodeFlags_None)) return;
        if (sessionDirectory.empty()) {
            ImGui::TextDisabled("Запустите видеопоток для создания логов сессии");
            return;
        }
        const auto path = utf8String(sessionDirectory);
        ImGui::TextDisabled("%s", path.c_str());
        if (ImGui::SmallButton("Копировать путь")) SDL_SetClipboardText(path.c_str());
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::SmallButton("Открыть папку")) {
            const auto value = reinterpret_cast<std::intptr_t>(
                ShellExecuteW(nullptr, L"open", sessionDirectory.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL));
            if (value <= 32) message = "Не удалось открыть папку сессии";
        }
#endif
        ImGui::SameLine();
        if (ImGui::SmallButton("Копировать логи")) SDL_SetClipboardText(output.c_str());
        // An expanded log viewer uses the unused vertical space on a
        // 1200x1920 screen instead of leaving a thousand empty pixels.
        const float logHeight = std::max(
            180.0f, ImGui::GetContentRegionAvail().y - 8.0f);
        ImGui::BeginChild("##log-text", ImVec2(0, logHeight),
                          ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(output.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 30.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    void draw() {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("##control-center", nullptr, flags);

        ImGui::TextColored(ImVec4(.91f,.95f,1,1), "ПАНЕЛЬ УПРАВЛЕНИЯ");
        ImGui::SameLine();
        const auto statusColor = process.running()
            ? ImVec4(.36f,.91f,.68f,1) : ImVec4(.65f,.72f,.84f,1);
        ImGui::TextColored(statusColor, "  %s  %s",
            process.running() ? Symbols::check : Symbols::info,
            process.running() ? "Работает" : "Ожидание запуска");
        ImGui::Separator();

        // Use the available viewport width. Columns adapt to the content
        // instead of masking an overly wide two-column layout with a 750px cap.
        // The footer is the only fixed-height region.
        ImGui::BeginChild("##scroll-content", ImVec2(0, -68),
                          ImGuiChildFlags_None);
        drawProfileBar();
#ifdef _WIN32
        drawMissionPlannerLayout();
#endif
        ImGui::Spacing();
        ImGui::BeginDisabled(process.running());
        drawSource();
        ImGui::EndDisabled();
        ImGui::Spacing();

        const float available = ImGui::GetContentRegionAvail().x;
        const auto columns = reg::launcher::dashboardColumns(available);
        const bool stopped = !process.running();

        if (columns == reg::launcher::DashboardColumns::three) {
            // Portrait 1200x1920: three compact cards across rather than two
            // wide cards with vast empty form rows. No card is scrollable.
            if (ImGui::BeginTable("##dashboard-three", 3,
                    ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawDisplays();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawRecorder();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawLatency();
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            ImGui::Spacing();
#ifdef _WIN32
            // Network and updater naturally occupy the width of the second row.
            if (ImGui::BeginTable("##dashboard-secondary", 2,
                    ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawNetwork();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                drawUpdates();
                ImGui::EndTable();
            }
#else
            ImGui::BeginDisabled(!stopped);
            drawNetwork();
            ImGui::EndDisabled();
#endif
        } else if (columns == reg::launcher::DashboardColumns::two) {
            if (ImGui::BeginTable("##dashboard-two", 2,
                    ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawDisplays();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawRecorder();
                ImGui::EndDisabled();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawNetwork();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!stopped);
                drawLatency();
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
#ifdef _WIN32
            ImGui::Spacing();
            drawUpdates();
#endif
        } else {
            // Narrow windows never squeeze settings into unreadable columns.
            ImGui::BeginDisabled(!stopped);
            drawDisplays();
            ImGui::Spacing();
            drawRecorder();
            ImGui::Spacing();
            drawNetwork();
            ImGui::Spacing();
            drawLatency();
            ImGui::EndDisabled();
#ifdef _WIN32
            ImGui::Spacing();
            drawUpdates();
#endif
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!stopped);
        drawAdvanced();
        ImGui::EndDisabled();

        ImGui::Spacing();
        drawLogs();
        ImGui::EndChild();

        ImGui::Separator();
        const ImVec4 action = process.running()
            ? ImVec4(.75f,.36f,.35f,1) : ImVec4(.19f,.69f,.48f,1);
        ImGui::PushStyleColor(ImGuiCol_Button, action);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(action.x+.10f, action.y+.08f, action.z+.08f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, action);
        if (ImGui::Button(process.running() ? "Остановить" : "Запустить",
                          ImVec2(180, 43))) {
            if (process.running()) {
                process.requestStop();
                message = "Останавливаем видеопоток...";
            } else {
                try { start(); }
                catch (const std::exception& e) { message = e.what(); }
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (process.running()) ImGui::TextUnformatted("Видео и телеметрия активны");
        else if (lastExit) ImGui::Text("Последнее завершение: %d", *lastExit);
        else ImGui::TextUnformatted("Готово к запуску");
        if (!message.empty()) {
            ImGui::TextDisabled("%s", message.c_str());
        } else {
            ImGui::TextDisabled("Сессии и настройки сохраняются автоматически");
        }
        ImGui::EndGroup();
        ImGui::End();
    }

};

ImFont* loadUiFont(const fs::path& path, float size,
                   const ImWchar* ranges, ImFontConfig* config = nullptr) {
    std::size_t length = 0;
    void* bytes = SDL_LoadFile(utf8String(path).c_str(), &length);
    if (!bytes || length == 0) return nullptr;
    void* atlasBytes = ImGui::MemAlloc(length);
    std::memcpy(atlasBytes, bytes, length);
    SDL_free(bytes);
    // ImGui owns and later frees atlasBytes.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        atlasBytes, static_cast<int>(length), size, config, ranges);
}

void loadFont(const fs::path& appDir) {
    auto folder = appDir / "fonts";
    if (!fs::is_regular_file(folder / "Roboto.ttf")) {
#ifdef REG_FONT_DIR
        folder = fs::path(REG_FONT_DIR);
#endif
    }
    const auto roboto = folder / "Roboto.ttf";
    const auto symbols = folder / "MaterialSymbolsOutlined.ttf";
    auto& io = ImGui::GetIO();
    if (fs::is_regular_file(roboto)) {
        static_cast<void>(loadUiFont(
            roboto, 17.0f, io.Fonts->GetGlyphRangesCyrillic()));
    }
    if (fs::is_regular_file(roboto) && fs::is_regular_file(symbols)) {
        ImFontConfig config{};
        config.MergeMode = true;
        config.PixelSnapH = true;
        static constexpr ImWchar ranges[] = {
        0xE002, 0xE002,
        0xE037, 0xE037,
        0xE047, 0xE047,
        0xE04B, 0xE04B,
        0xE145, 0xE145,
        0xE161, 0xE161,
        0xE1DB, 0xE1DB,
        0xE2C4, 0xE2C4,
        0xE2C7, 0xE2C7,
        0xE333, 0xE333,
        0xE429, 0xE429,
        0xE5D5, 0xE5D5,
        0xE63E, 0xE63E,
        0xE86C, 0xE86C,
        0xE86F, 0xE86F,
        0xE88E, 0xE88E,
        0xE8B8, 0xE8B8,
            0
        };
        static_cast<void>(loadUiFont(symbols, 20.0f, ranges, &config));
    }
}

void applyTheme() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(17, 12);
    style.FramePadding = ImVec2(9, 6);
    style.ItemSpacing = ImVec2(9, 7);
    style.ChildRounding = 11.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    auto* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(.065f,.086f,.12f,1);
    colors[ImGuiCol_Text] = ImVec4(.92f,.95f,.99f,1);
    colors[ImGuiCol_TextDisabled] = ImVec4(.59f,.65f,.75f,1);
    colors[ImGuiCol_Border] = ImVec4(.23f,.30f,.39f,1);
    colors[ImGuiCol_FrameBg] = ImVec4(.13f,.18f,.23f,1);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(.18f,.24f,.32f,1);
    colors[ImGuiCol_FrameBgActive] = ImVec4(.21f,.30f,.41f,1);
    colors[ImGuiCol_Header] = ImVec4(.18f,.30f,.39f,1);
    colors[ImGuiCol_HeaderHovered] = ImVec4(.21f,.37f,.48f,1);
    colors[ImGuiCol_Button] = ImVec4(.18f,.31f,.40f,1);
    colors[ImGuiCol_ButtonHovered] = ImVec4(.22f,.43f,.56f,1);
    colors[ImGuiCol_CheckMark] = ImVec4(.43f,.88f,.76f,1);
    colors[ImGuiCol_SliderGrab] = ImVec4(.43f,.78f,.97f,1);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(.57f,.86f,1,1);
    colors[ImGuiCol_Separator] = ImVec4(.20f,.26f,.34f,1);
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "Панель управления", 960, 840,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window) SDL_SetWindowMinimumSize(window, 720, 650);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

#ifdef _WIN32
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "direct3d11");
#else
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
#endif
    if (!renderer) renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyTheme();
    ImGui::GetIO().IniFilename = nullptr;
    loadFont(reg::launcher::executableDirectory());
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    int exitCode = 0;
    try {
        Ui ui(window);
        bool done = false;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    done = true;
                }
            }
            if (done) break;
            ui.update();
#ifdef _WIN32
            if (ui.quitForUpdate) break;
#endif
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ui.draw();
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 20, 23, 30, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Launcher error: %s\n", error.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ошибка запуска",
                                 error.what(), window);
        exitCode = 1;
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
