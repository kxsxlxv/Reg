#include "app/RuntimeUiState.hpp"
#include "platform/SDLPlatform.hpp"
#include "platform/DisplayRouting.hpp"
#include "remote/RemoteTextFocusGate.hpp"

#include <SDL3/SDL.h>

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
#include "platform/SdlImGuiKeyMap.hpp"
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace reg::platform {
namespace {

const char* localizedWindowTitle(const char* title) noexcept {
    if (title == nullptr) {
        return "";
    }

    const std::string_view value{title};
    if (value == "Reg - Raw") {
        return "Reg — Raw";
    }
    if (value == "Reg - Exact CV Overlay") {
        return "Reg — CV Overlay (точный)";
    }
    if (value == "Reg - Telemetry") {
        return "Reg — Телеметрия";
    }
    if (value == "Reg Replay - Raw") {
        return "Reg Replay — Raw";
    }
    if (value == "Reg Replay - Exact Overlay") {
        return "Reg Replay — Overlay (точный)";
    }
    return title;
}

const char* tracedWindowEventName(std::uint32_t type) noexcept {
    switch (type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "FOCUS_GAINED";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "FOCUS_LOST";
    case SDL_EVENT_WINDOW_EXPOSED:
        return "EXPOSED";
    case SDL_EVENT_WINDOW_RESIZED:
        return "RESIZED";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        return "PIXEL_SIZE_CHANGED";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        return "DISPLAY_CHANGED";
    default:
        return nullptr;
    }
}

void traceWindowEvent(const SDL_Event& event) {
    const char* name = tracedWindowEventName(event.type);
    if (name == nullptr) {
        return;
    }

    SDL_Window* window =
        SDL_GetWindowFromID(event.window.windowID);
    const char* title =
        window != nullptr
            ? SDL_GetWindowTitle(window)
            : nullptr;
    const SDL_WindowFlags flags =
        window != nullptr
            ? SDL_GetWindowFlags(window)
            : 0U;

    std::cerr
        << "[window-event] "
        << name
        << " id="
        << event.window.windowID
        << " title=\""
        << (title != nullptr ? title : "<unknown>")
        << "\" data="
        << event.window.data1
        << 'x'
        << event.window.data2
        << " flags=0x"
        << std::hex
        << static_cast<unsigned long long>(flags)
        << std::dec
        << '\n';
}

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
bool isRegLocalHotkey(SDL_Keycode key) noexcept {
    return key == SDLK_F10 || key == SDLK_F11 || key == SDLK_F12;
}

void updateRemoteModifierKeys(
    remote::RemoteUiInputState& state,
    SDL_Keymod mods) noexcept {
    remote::setRemoteUiKey(
        state,
        ImGuiKey_ReservedForModCtrl,
        (mods & SDL_KMOD_CTRL) != 0);
    remote::setRemoteUiKey(
        state,
        ImGuiKey_ReservedForModShift,
        (mods & SDL_KMOD_SHIFT) != 0);
    remote::setRemoteUiKey(
        state,
        ImGuiKey_ReservedForModAlt,
        (mods & SDL_KMOD_ALT) != 0);
    remote::setRemoteUiKey(
        state,
        ImGuiKey_ReservedForModSuper,
        (mods & SDL_KMOD_GUI) != 0);
}

int remoteMouseButtonIndex(std::uint8_t button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

void appendRemoteTextUtf8(
    remote::RemoteUiInputState& state,
    const char* text) noexcept {
    if (text == nullptr) {
        return;
    }

    const auto* cursor =
        reinterpret_cast<const unsigned char*>(text);
    while (*cursor != 0U &&
           state.textCount < remote::kRemoteUiTextCapacity) {
        std::uint32_t codepoint = 0xFFFDU;
        std::size_t length = 1U;
        const unsigned char lead = cursor[0];

        if (lead < 0x80U) {
            codepoint = lead;
        } else if ((lead & 0xE0U) == 0xC0U &&
                   cursor[1] != 0U &&
                   (cursor[1] & 0xC0U) == 0x80U) {
            codepoint =
                (static_cast<std::uint32_t>(lead & 0x1FU) << 6U) |
                static_cast<std::uint32_t>(cursor[1] & 0x3FU);
            length = 2U;
            if (codepoint < 0x80U) {
                codepoint = 0xFFFDU;
                length = 1U;
            }
        } else if ((lead & 0xF0U) == 0xE0U &&
                   cursor[1] != 0U && cursor[2] != 0U &&
                   (cursor[1] & 0xC0U) == 0x80U &&
                   (cursor[2] & 0xC0U) == 0x80U) {
            codepoint =
                (static_cast<std::uint32_t>(lead & 0x0FU) << 12U) |
                (static_cast<std::uint32_t>(cursor[1] & 0x3FU) << 6U) |
                static_cast<std::uint32_t>(cursor[2] & 0x3FU);
            length = 3U;
            if (codepoint < 0x800U ||
                (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
                codepoint = 0xFFFDU;
                length = 1U;
            }
        } else if ((lead & 0xF8U) == 0xF0U &&
                   cursor[1] != 0U && cursor[2] != 0U &&
                   cursor[3] != 0U &&
                   (cursor[1] & 0xC0U) == 0x80U &&
                   (cursor[2] & 0xC0U) == 0x80U &&
                   (cursor[3] & 0xC0U) == 0x80U) {
            // NetImgui CmdInput carries 16-bit characters. Keep BMP text exact
            // and use the replacement glyph for codepoints outside that range.
            length = 4U;
            codepoint = 0xFFFDU;
        }

        state.text[state.textCount++] =
            static_cast<std::uint16_t>(codepoint);
        cursor += length;
    }
}
#endif

bool placeBorderlessWindow(
    SDL_Window* window,
    int x,
    int y,
    int width,
    int height) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & SDL_WINDOW_FULLSCREEN) != 0U &&
        !SDL_SetWindowFullscreen(window, false)) {
        return false;
    }

    // Deliberately use a normal borderless window rather than SDL fullscreen.
    // On Windows a real fullscreen state can cause WSI/swapchain transitions.
    // Focus ownership is independent from pointer interaction: secondary
    // output windows can stay non-focusable while still receiving mouse events.
    if (!SDL_SetWindowBordered(window, false)) {
        return false;
    }
    if (!SDL_SetWindowResizable(window, false)) {
        return false;
    }
    if (!SDL_SetWindowPosition(window, x, y)) {
        return false;
    }
    if (!SDL_SetWindowSize(window, width, height)) {
        return false;
    }
    return SDL_SyncWindow(window);
}

#ifdef _WIN32
struct WindowsSettingsDisplay final {
    int settingsNumber{};
    int x{};
    int y{};
    int width{};
    int height{};
};

struct MonitorRectLookup final {
    std::wstring deviceName;
    std::optional<RECT> rect;
};

BOOL CALLBACK monitorRectCallback(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM userData) {
    auto* lookup =
        reinterpret_cast<MonitorRectLookup*>(userData);
    if (lookup == nullptr) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(
            monitor,
            reinterpret_cast<MONITORINFO*>(&info))) {
        return TRUE;
    }

    if (lstrcmpiW(
            info.szDevice,
            lookup->deviceName.c_str()) != 0) {
        return TRUE;
    }

    lookup->rect = info.rcMonitor;
    return FALSE;
}

std::optional<RECT> desktopMonitorRectForPath(
    const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
    sourceName.header.type =
        DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;

    if (DisplayConfigGetDeviceInfo(
            &sourceName.header) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    MonitorRectLookup lookup{
        .deviceName = sourceName.viewGdiDeviceName,
        .rect = std::nullopt,
    };

    static_cast<void>(
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            monitorRectCallback,
            reinterpret_cast<LPARAM>(&lookup)));

    return lookup.rect;
}

std::vector<WindowsSettingsDisplay> windowsSettingsDisplays() {
    UINT32 pathCount = 0U;
    UINT32 modeCount = 0U;

    LONG result = GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        &modeCount);
    if (result != ERROR_SUCCESS) {
        return {};
    }

    // The topology can change between the size query and QueryDisplayConfig.
    // Retry on ERROR_INSUFFICIENT_BUFFER instead of using stale array sizes.
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

        UINT32 actualPathCount = pathCount;
        UINT32 actualModeCount = modeCount;
        result = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &actualPathCount,
            paths.data(),
            &actualModeCount,
            modes.data(),
            nullptr);

        if (result == ERROR_INSUFFICIENT_BUFFER) {
            result = GetDisplayConfigBufferSizes(
                QDC_ONLY_ACTIVE_PATHS,
                &pathCount,
                &modeCount);
            if (result != ERROR_SUCCESS) {
                return {};
            }
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return {};
        }

        paths.resize(actualPathCount);
        modes.resize(actualModeCount);

        std::vector<WindowsSettingsDisplay> displays;
        displays.reserve(paths.size());

        // Windows Settings "Identify" numbering follows the active CCD path
        // order: paths[0] -> display 1, paths[1] -> display 2, ... . Do not use
        // SDL enumeration or the GDI \\.\DISPLAYn suffix for numbering.
        //
        // The GDI monitor rectangle is still used for geometry after a path is
        // identified. It is expressed in the actual desktop coordinate space
        // and therefore already contains the effective portrait/landscape
        // orientation. Using DISPLAYCONFIG_SOURCE_MODE width/height directly
        // can describe the source surface rather than the rotated desktop
        // rectangle on a portrait target.
        for (std::size_t index = 0U; index < paths.size(); ++index) {
            const auto& path = paths[index];
            const UINT32 modeIndex = path.sourceInfo.modeInfoIdx;
            if (modeIndex == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                modeIndex >= modes.size()) {
                continue;
            }

            const auto& mode = modes[modeIndex];
            if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                continue;
            }

            const auto monitorRect =
                desktopMonitorRectForPath(path);
            if (monitorRect.has_value()) {
                displays.push_back(
                    WindowsSettingsDisplay{
                        .settingsNumber = static_cast<int>(index + 1U),
                        .x = monitorRect->left,
                        .y = monitorRect->top,
                        .width = monitorRect->right - monitorRect->left,
                        .height = monitorRect->bottom - monitorRect->top,
                    });
                continue;
            }

            // Conservative fallback for unusual display drivers where the CCD
            // source cannot be mapped back to an HMONITOR.
            const auto& source = mode.sourceMode;
            displays.push_back(
                WindowsSettingsDisplay{
                    .settingsNumber = static_cast<int>(index + 1U),
                    .x = source.position.x,
                    .y = source.position.y,
                    .width = static_cast<int>(source.width),
                    .height = static_cast<int>(source.height),
                });
        }
        return displays;
    }

    return {};
}

#endif

} // namespace

SDLPlatform::SDLPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(
            std::string("SDL_Init failed: ") +
            SDL_GetError());
    }
}

SDLPlatform::~SDLPlatform() {
    for (SDL_Window* window : windows_) {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
    windows_.clear();
    SDL_Quit();
}

SDL_Window* SDLPlatform::createVulkanWindow(
    const char* title,
    int width,
    int height,
    bool focusable) {
    SDL_WindowFlags flags =
        SDL_WINDOW_VULKAN |
        SDL_WINDOW_RESIZABLE;

    if (!focusable) {
        flags |= SDL_WINDOW_NOT_FOCUSABLE;
    }

    SDL_Window* window = SDL_CreateWindow(
        localizedWindowTitle(title),
        width,
        height,
        flags);

    if (window == nullptr) {
        throw std::runtime_error(
            std::string("SDL_CreateWindow failed: ") +
            SDL_GetError());
    }

    if (!focusable &&
        !SDL_SetWindowFocusable(window, false)) {
        const std::string error = SDL_GetError();
        SDL_DestroyWindow(window);
        throw std::runtime_error(
            std::string("SDL_SetWindowFocusable(false) failed: ") +
            error);
    }

    if (windows_.empty()) {
        rawWindowId_ = static_cast<std::uint32_t>(
            SDL_GetWindowID(window));
    }

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    if (windows_.empty()) {
        if (!SDL_StartTextInput(window)) {
            std::cerr
                << "[netimgui] SDL_StartTextInput(raw) failed: "
                << SDL_GetError()
                << '\n';
        }
    }
#endif

    windows_.push_back(window);
    return window;
}

std::vector<DisplayInfo>
SDLPlatform::displays() const {
    int count = 0;
    SDL_DisplayID* ids =
        SDL_GetDisplays(&count);

    if (ids == nullptr) {
        throw std::runtime_error(
            std::string("SDL_GetDisplays failed: ") +
            SDL_GetError());
    }

    std::vector<DisplayInfo> result;
    result.reserve(
        static_cast<std::size_t>(
            std::max(count, 0)));

    for (int i = 0; i < count; ++i) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(
                ids[i],
                &bounds)) {
            SDL_free(ids);
            throw std::runtime_error(
                std::string("SDL_GetDisplayBounds failed: ") +
                SDL_GetError());
        }

        const char* name =
            SDL_GetDisplayName(ids[i]);

        result.push_back(
            DisplayInfo{
                .id =
                    static_cast<std::uint32_t>(
                        ids[i]),
                .name =
                    name != nullptr
                        ? std::string(name)
                        : std::string("Unknown"),
                .x = bounds.x,
                .y = bounds.y,
                .width = bounds.w,
                .height = bounds.h,
            });
    }

    SDL_free(ids);
    return result;
}

bool SDLPlatform::placeWindowOnDisplay(
    SDL_Window* window,
    std::size_t logicalRole,
    int displayNumberOverride,
    int reservedDisplayNumber) {
    if (window == nullptr) {
        throw std::invalid_argument(
            "placeWindowOnDisplay requires a window");
    }

    // Call sites use logical roles in creation order:
    //   0 = Raw, 1 = Overlay, 2 = Telemetry.
    // These numbers are the numbers shown by Windows Settings -> Display ->
    // Identify, not SDL ordinals and not GDI \\.\DISPLAYn suffixes.
    constexpr std::array<int, 3> roleToWindowsSettingsDisplay{3, 2, 1};

#ifdef _WIN32
    const auto displays = windowsSettingsDisplays();
    std::vector<int> active;
    active.reserve(displays.size());
    for (const auto& display : displays) {
        active.push_back(display.settingsNumber);
    }
    const int requested = displayNumberOverride > 0
        ? displayNumberOverride
        : (logicalRole < roleToWindowsSettingsDisplay.size()
            ? roleToWindowsSettingsDisplay[logicalRole] : 0);
    const int selected = selectUsableDisplay(
        requested, reservedDisplayNumber, active);
    if (selected == 0) return false;
    const auto display = std::ranges::find_if(
        displays, [selected](const WindowsSettingsDisplay& value) {
            return value.settingsNumber == selected;
        });
    if (display == displays.end()) return false;
    if (selected != requested) {
        std::cerr << "[display] monitor " << requested
                  << " unavailable/reserved; using " << selected << '\n';
    }
    return placeBorderlessWindow(
        window, display->x, display->y, display->width, display->height);
#else
    // SDL ordinal numbering on non-Windows hosts. Never place a window on
    // the Jetson-reserved output even if Windows monitor numbering differs.
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (!ids) return false;
    std::vector<int> active;
    for (int i = 0; i < count; ++i) active.push_back(i + 1);
    SDL_free(ids);
    const int requested = displayNumberOverride > 0
        ? displayNumberOverride : static_cast<int>(logicalRole) + 1;
    const int selected = selectUsableDisplay(
        requested, reservedDisplayNumber, active);
    if (selected == 0) return false;
    return placeWindowFullscreenOnDisplay(
        window, static_cast<std::size_t>(selected - 1));
#endif
}

bool SDLPlatform::placeWindowFullscreenOnDisplay(
    SDL_Window* window,
    std::size_t displayOrdinal) {
    if (window == nullptr) {
        throw std::invalid_argument(
            "placeWindowFullscreenOnDisplay requires a window");
    }

    const auto connected = displays();
    if (displayOrdinal >= connected.size()) {
        return false;
    }

    const auto& display = connected[displayOrdinal];
    return placeBorderlessWindow(
        window,
        display.x,
        display.y,
        display.width,
        display.height);
}

bool SDLPlatform::pollQuitRequested() {
    SDL_Event event{};

    while (SDL_PollEvent(&event)) {
        traceWindowEvent(event);

        if (event.type ==
                SDL_EVENT_QUIT ||
            event.type ==
                SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            return true;
        }

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
        if (rawWindowId_ != 0U) {
            if (event.type == SDL_EVENT_MOUSE_MOTION &&
                event.motion.windowID == rawWindowId_) {
                remoteUiInput_.mouseX =
                    static_cast<std::int32_t>(event.motion.x);
                remoteUiInput_.mouseY =
                    static_cast<std::int32_t>(event.motion.y);
            } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                       event.button.windowID == rawWindowId_) {
                remoteUiInput_.mouseX =
                    static_cast<std::int32_t>(event.button.x);
                remoteUiInput_.mouseY =
                    static_cast<std::int32_t>(event.button.y);
                const int button =
                    remoteMouseButtonIndex(event.button.button);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    remote::gRemoteTextFocus.load(std::memory_order_relaxed) !=
                        remote::RemoteTextFocus::Disconnected) {
                    lastRawRemoteFocusChange_ = std::chrono::steady_clock::now();
                }
                if (button >= 0) {
                    const std::uint64_t mask =
                        std::uint64_t{1} <<
                        static_cast<unsigned>(button);
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        remoteUiInput_.mouseDownMask |= mask;
                    } else {
                        remoteUiInput_.mouseDownMask &= ~mask;
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL &&
                       event.wheel.windowID == rawWindowId_) {
                // Match Dear ImGui's SDL3 backend horizontal-wheel convention.
                remoteUiInput_.wheelX += -event.wheel.x;
                remoteUiInput_.wheelY += event.wheel.y;
            } else if ((event.type == SDL_EVENT_KEY_DOWN ||
                        event.type == SDL_EVENT_KEY_UP) &&
                       event.key.windowID == rawWindowId_) {
                if (event.type == SDL_EVENT_KEY_DOWN &&
                    event.key.scancode == SDL_SCANCODE_TAB &&
                    remote::gRemoteTextFocus.load(std::memory_order_relaxed) !=
                        remote::RemoteTextFocus::Disconnected) {
                    lastRawRemoteFocusChange_ = std::chrono::steady_clock::now();
                }
                updateRemoteModifierKeys(
                    remoteUiInput_,
                    event.key.mod);
                if (!isRegLocalHotkey(event.key.key) &&
                    !(event.key.scancode == SDL_SCANCODE_SPACE &&
                      (event.key.mod & SDL_KMOD_SHIFT) != 0)) {
                    remote::setRemoteUiKey(
                        remoteUiInput_,
                        sdlKeyToImGuiKey(
                            event.key.key,
                            event.key.scancode),
                        event.type == SDL_EVENT_KEY_DOWN);
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT &&
                       event.text.windowID == rawWindowId_) {
                appendRemoteTextUtf8(
                    remoteUiInput_,
                    event.text.text);
            } else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE &&
                       event.window.windowID == rawWindowId_) {
                remoteUiInput_.mouseX =
                    remote::kRemoteUiMouseUnavailable;
                remoteUiInput_.mouseY =
                    remote::kRemoteUiMouseUnavailable;
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
                       event.window.windowID == rawWindowId_) {
                remoteUiInput_.mouseX =
                    remote::kRemoteUiMouseUnavailable;
                remoteUiInput_.mouseY =
                    remote::kRemoteUiMouseUnavailable;
                remote::clearRemoteUiPressedState(remoteUiInput_);
            }
        }
#endif

        // Freeze/unfreeze only the visible GT range history, not recording
        // or real-time telemetry collection. Ignore repeats and text editing.
        if (event.type == SDL_EVENT_KEY_DOWN &&
            !event.key.repeat &&
            event.key.scancode == SDL_SCANCODE_SPACE &&
            event.key.windowID == rawWindowId_ &&
            (event.key.mod & SDL_KMOD_SHIFT) != 0 &&
            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
            remote::gRemoteTextFocus.load(std::memory_order_relaxed) !=
                remote::RemoteTextFocus::Editing {
            app::toggleRangeTimelinePaused();
        }

        // Physical QWERTY T (Russian layout: Е). Only Raw's keyboard
        // focus is eligible; never repeat or intercept modifier shortcuts.
        // SDL's IME composition is handled separately below.
        if (event.type == SDL_EVENT_KEY_DOWN &&
            !event.key.repeat &&
            event.key.windowID == rawWindowId_ &&
            event.key.scancode == SDL_SCANCODE_T &&
            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
            !gtImeComposing_ &&
            remote::gtHotkeyAllowed(remote::gRemoteTextFocus.load(
                std::memory_order_relaxed)) &&
            std::chrono::steady_clock::now() - lastRawRemoteFocusChange_ >=
                std::chrono::milliseconds{250}) {
            app::toggleGtBoxes();
        }

        if (event.type == SDL_EVENT_TEXT_EDITING &&
            event.edit.windowID == rawWindowId_) {
            gtImeComposing_ = event.edit.text != nullptr && event.edit.text[0] != '\0';
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
            event.window.windowID == rawWindowId_) {
            gtImeComposing_ = false;
        }

        if (event.type ==
                SDL_EVENT_KEY_DOWN &&
            !event.key.repeat) {
            if (event.key.key == SDLK_F10) {
                app::toggleTelemetryPage();
                std::cout
                    << "[telemetry] page="
                    << (app::telemetryPage() == app::TelemetryPage::NetImgui
                            ? "NetImgui"
                            : "Overview")
                    << '\n';
            } else if (event.key.key == SDLK_F11) {
                rawScreenshotRequested_ = true;
            } else if (
                event.key.key == SDLK_F12) {
                overlayScreenshotRequested_ = true;
            }
        }

        // A window merely changing its current display/focus is not a physical
        // topology change and must not cause all three borderless windows to be
        // repositioned/recreated. Only actual display topology events trigger
        // remapping.
        if (event.type ==
                SDL_EVENT_DISPLAY_ADDED ||
            event.type ==
                SDL_EVENT_DISPLAY_REMOVED ||
            event.type ==
                SDL_EVENT_DISPLAY_MOVED) {
            displayTopologyChanged_ = true;
        }
    }

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    remote::publishRemoteUiInput(remoteUiInput_);
    remote::clearRemoteUiTransient(remoteUiInput_);
#endif

    return false;
}

} // namespace reg::platform
