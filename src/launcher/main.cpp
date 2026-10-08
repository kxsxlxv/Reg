#include "launcher/LauncherConfig.hpp"
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

struct Ui {
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
    reg::launcher::UpdateManager updater{reg::launcher::executableDirectory()};
    std::string updateChannel{"dev"};
    bool quitForUpdate{false};
    bool autoUpdateStarted{false};
#endif

    Ui() {
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
        float height = info.phase == UpdatePhase::Downloading ? 150.0f : 115.0f;
        if (card("##updates-card", height, ImVec4(.118f,.143f,.192f,1),
                 Symbols::refresh, "Обновления", ImVec4(.54f,.72f,1,1))) {
            const bool busy = info.phase == UpdatePhase::Checking ||
                              info.phase == UpdatePhase::Downloading;
            ImGui::BeginDisabled(busy);
            ImGui::SetNextItemWidth(160);
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
    static bool card(const char* id, float height, ImVec4 background,
                     const char* symbol, const char* label, ImVec4 accent) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.34f));
        const bool visible = ImGui::BeginChild(
            id, ImVec2(0, height), ImGuiChildFlags_Borders);
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

    static void integer(const char* label, const char* id, int& value) {
        caption(label);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt(id, &value);
    }

    static int connectedDisplays() {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        if (displays) SDL_free(displays);
        return std::clamp(count, 1, 16);
    }

    void displaySelector(const char* id, const char* label,
                         int& selection, const char* defaultLabel) {
        caption(label);
        const int count = connectedDisplays();
        std::string preview = selection == 0
            ? std::string("Автоматически (") + defaultLabel + ")"
            : "Монитор " + std::to_string(selection);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(id, preview.c_str())) {
            if (ImGui::Selectable("Автоматически##auto", selection == 0)) selection = 0;
            for (int number = 1; number <= count; ++number) {
                const std::string item = "Монитор " + std::to_string(number);
                if (ImGui::Selectable(item.c_str(), selection == number)) {
                    selection = number;
                }
            }
            ImGui::EndCombo();
        }
    }

    void drawProfileBar() {
        ImGui::TextDisabled("Профиль запуска");
        const float available = ImGui::GetContentRegionAvail().x;
        const float selectorWidth = std::max(140.0f, available - 265.0f);
        ImGui::SetNextItemWidth(selectorWidth);
        if (ImGui::BeginCombo("##profile", profileName.c_str())) {
            for (const auto& name : profileNames) {
                if (ImGui::Selectable(name.c_str(), name == profileName)) {
                    try { selectProfile(name); }
                    catch (const std::exception& e) { message = e.what(); }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(process.running());
        if (ImGui::Button("Сохранить", ImVec2(110, 0))) {
            try { save(); } catch (const std::exception& e) { message = e.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Новый профиль", ImVec2(145, 0))) {
            showCreateProfile = !showCreateProfile;
        }
        ImGui::EndDisabled();

        if (showCreateProfile) {
            ImGui::SetNextItemWidth(-155);
            ImGui::InputTextWithHint("##new-name", "Имя латиницей, цифры, - или _",
                                     newName.data(), newName.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(process.running());
            if (ImGui::Button("Создать", ImVec2(135, 0))) {
                try {
                    createProfile();
                    showCreateProfile = false;
                } catch (const std::exception& e) { message = e.what(); }
            }
            ImGui::EndDisabled();
        }
    }

    void drawSource() {
        if (card("##source-card", 116.0f, ImVec4(0.105f, 0.151f, 0.195f, 1),
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
        if (card("##display-card", 283.0f, ImVec4(0.109f, 0.157f, 0.163f, 1),
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
        if (card("##record-card", 283.0f, ImVec4(0.155f, 0.125f, 0.167f, 1),
                 Symbols::record, "Запись Blackbox", ImVec4(0.91f, 0.65f, 0.94f, 1))) {
            ImGui::Checkbox("Включить циклическую запись", &profile.recorderEnabled);
            ImGui::BeginDisabled(!profile.recorderEnabled);
            integer("Длительность сегмента, мс", "##record-segment", profile.recordSegmentMs);
            caption("Срок хранения записей");
            int minutes = std::max(1, profile.recordRetentionSec / 60);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##retention-minutes", &minutes, 1, 5)) {
                profile.recordRetentionSec = std::clamp(minutes, 1, 100000) * 60;
            }
            ImGui::TextDisabled("минуты  ·  файлы в папке recordings/blackbox");
            ImGui::EndDisabled();
        }
        finishCard();
    }

    void drawNetwork() {
        if (card("##network-card", 215.0f, ImVec4(0.124f, 0.143f, 0.197f, 1),
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
        if (card("##latency-card", 215.0f, ImVec4(0.176f, 0.140f, 0.114f, 1),
                 Symbols::tune, "Задержка и восстановление", ImVec4(1.0f, 0.73f, 0.40f, 1))) {
            caption("Задержка CV Overlay, мс");
            ImGui::SetNextItemWidth(-1);
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
        ImGui::BeginChild("##log-text", ImVec2(0, 180), ImGuiChildFlags_Borders);
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

        // Reserve a non-scrolling footer for the primary action and status.
        ImGui::BeginChild("##scroll-content", ImVec2(0, -75),
                          ImGuiChildFlags_None);
        ImGui::BeginDisabled(process.running());
        drawProfileBar();
        ImGui::Spacing();
        drawSource();
        ImGui::Spacing();
        if (ImGui::GetContentRegionAvail().x >= 750.0f) {
            if (ImGui::BeginTable("##main-cards", 2,
                                 ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                drawDisplays();
                ImGui::TableNextColumn();
                drawRecorder();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawNetwork();
                ImGui::TableNextColumn();
                drawLatency();
                ImGui::EndTable();
            }
        } else {
            drawDisplays();
            drawRecorder();
            drawNetwork();
            drawLatency();
        }
        ImGui::Spacing();
        drawAdvanced();
        ImGui::EndDisabled();

#ifdef _WIN32
        ImGui::Spacing();
        drawUpdates();
#endif
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
        "Панель управления", 1040, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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
        Ui ui;
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
