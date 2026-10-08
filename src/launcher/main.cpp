#include "launcher/LauncherConfig.hpp"
#include "launcher/ProcessManager.hpp"

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
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using reg::launcher::Profile;

template <std::size_t N>
void copyTo(std::array<char, N>& buffer, const std::string& source) {
    std::snprintf(buffer.data(), buffer.size(), "%s", source.c_str());
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

    Ui() {
        profileNames = reg::launcher::listProfiles();
        auto selected = reg::launcher::lastSelectedProfile();
        if (std::find(profileNames.begin(), profileNames.end(), selected) == profileNames.end()) {
            selected = profileNames.front();
        }
        selectProfile(selected);
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

    void draw() {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(io.DisplaySize);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("Панель управления", nullptr, flags);

        ImGui::TextUnformatted("Панель управления");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginCombo("Профиль", profileName.c_str())) {
            for (const auto& name : profileNames) {
                if (ImGui::Selectable(name.c_str(), name == profileName)) {
                    try { selectProfile(name); }
                    catch (const std::exception& e) { message = e.what(); }
                }
            }
            ImGui::EndCombo();
        }
        if (!process.running()) {
            ImGui::SameLine();
            if (ImGui::Button("Сохранить")) {
                try { save(); }
                catch (const std::exception& e) { message = e.what(); }
            }
        }

        ImGui::BeginDisabled(process.running());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##rtsp", url.data(), url.size());
        ImGui::TextDisabled("RTSP источник");

        ImGui::Checkbox("CV Overlay", &profile.overlayEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Telemetry", &profile.telemetryEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Blackbox", &profile.recorderEnabled);

        ImGui::SliderInt("Задержка Overlay, мс", &profile.overlayDelayMs, 0, 1000);
        ImGui::InputInt("UDP порт метаданных", &profile.metadataPort);
        ImGui::Checkbox("Дополнительные настройки", &showAdvanced);
        if (showAdvanced) {
            ImGui::SeparatorText("RTSP / Decode");
            ImGui::InputInt("max_delay, мкс", &profile.maxDelayUs);
            ImGui::InputInt("RTP reorder queue", &profile.reorderQueueSize);
            ImGui::InputInt("Дополнительные GPU frames", &profile.extraHwFrames);
            ImGui::InputText("UDP bind", bind.data(), bind.size());

            ImGui::SeparatorText("Recorder");
            ImGui::InputInt("Сегмент, мс", &profile.recordSegmentMs);
            ImGui::InputInt("Хранение, сек", &profile.recordRetentionSec);
            ImGui::InputInt("Очередь записи", &profile.recordQueueCapacity);

            ImGui::SeparatorText("Диагностика");
            ImGui::Checkbox("Vulkan validation", &profile.validationEnabled);
            ImGui::Checkbox("Требовать frame identity", &profile.requireFrameIdentity);
            ImGui::InputInt("Identity probe frames (0 = выкл)", &profile.identityProbeFrames);
            ImGui::InputInt("Reconnect initial, мс", &profile.reconnectInitialMs);
            ImGui::InputInt("Reconnect max, мс", &profile.reconnectMaxMs);
            ImGui::SeparatorText("Создать профиль");
            ImGui::InputText("Имя (латиница, цифры, - и _)", newName.data(), newName.size());
            if (ImGui::Button("Сохранить как новый")) {
                try { createProfile(); }
                catch (const std::exception& e) { message = e.what(); }
            }
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        if (!process.running()) {
            if (ImGui::Button("Запустить", ImVec2(160.0f, 38.0f))) {
                try { start(); }
                catch (const std::exception& e) { message = e.what(); }
            }
        } else {
            if (ImGui::Button("Остановить", ImVec2(160.0f, 38.0f))) {
                process.requestStop();
                message = "Ожидание корректного завершения...";
            }
        }
        ImGui::SameLine();
        if (process.running()) ImGui::TextUnformatted("● Работает");
        else if (lastExit) ImGui::Text("Завершено: %d", *lastExit);
        else ImGui::TextUnformatted("Остановлено");
        if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());

        ImGui::SeparatorText("Логи текущей сессии");
        if (!sessionDirectory.empty()) {
            const auto utf8Path = sessionDirectory.u8string();
            const std::string path(
                reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
            ImGui::TextWrapped("%s", path.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Копировать путь")) SDL_SetClipboardText(path.c_str());
            if (ImGui::SmallButton("Копировать логи")) SDL_SetClipboardText(output.c_str());
        }
        if (ImGui::BeginChild("##logs", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted(output.c_str());
        }
        ImGui::EndChild();
        ImGui::End();
    }
};

void loadFont(const fs::path& appDir) {
    auto candidate = appDir / "fonts" / "Roboto.ttf";
    if (!fs::exists(candidate)) {
#ifdef REG_FONT_DIR
        candidate = fs::path(REG_FONT_DIR) / "Roboto.ttf";
#endif
    }
    if (fs::exists(candidate)) {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), 19.0f, nullptr,
                                      io.Fonts->GetGlyphRangesCyrillic());
    }
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "Панель управления", 1000, 820,
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
    ImGui::StyleColorsDark();
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
