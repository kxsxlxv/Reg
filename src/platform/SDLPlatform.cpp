#include "app/RuntimeUiState.hpp"
#include "platform/SDLPlatform.hpp"

#include <SDL3/SDL.h>

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

ImGuiKey sdlKeyToImGuiKey(
    SDL_Keycode keycode,
    SDL_Scancode scancode) noexcept {
    // Keep this mapping aligned with the pinned Dear ImGui SDL3 backend.
    switch (scancode) {
    case SDL_SCANCODE_KP_0: return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_1: return ImGuiKey_Keypad1;
    case SDL_SCANCODE_KP_2: return ImGuiKey_Keypad2;
    case SDL_SCANCODE_KP_3: return ImGuiKey_Keypad3;
    case SDL_SCANCODE_KP_4: return ImGuiKey_Keypad4;
    case SDL_SCANCODE_KP_5: return ImGuiKey_Keypad5;
    case SDL_SCANCODE_KP_6: return ImGuiKey_Keypad6;
    case SDL_SCANCODE_KP_7: return ImGuiKey_Keypad7;
    case SDL_SCANCODE_KP_8: return ImGuiKey_Keypad8;
    case SDL_SCANCODE_KP_9: return ImGuiKey_Keypad9;
    case SDL_SCANCODE_KP_PERIOD: return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE: return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS: return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS: return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS: return ImGuiKey_KeypadEqual;
    default: break;
    }

    switch (keycode) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_COMMA: return ImGuiKey_Comma;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
    case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDLK_SCROLLLOCK: return ImGuiKey_ScrollLock;
    case SDLK_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDLK_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDLK_PAUSE: return ImGuiKey_Pause;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_LGUI: return ImGuiKey_LeftSuper;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_RGUI: return ImGuiKey_RightSuper;
    case SDLK_APPLICATION: return ImGuiKey_Menu;
    case SDLK_0: return ImGuiKey_0;
    case SDLK_1: return ImGuiKey_1;
    case SDLK_2: return ImGuiKey_2;
    case SDLK_3: return ImGuiKey_3;
    case SDLK_4: return ImGuiKey_4;
    case SDLK_5: return ImGuiKey_5;
    case SDLK_6: return ImGuiKey_6;
    case SDLK_7: return ImGuiKey_7;
    case SDLK_8: return ImGuiKey_8;
    case SDLK_9: return ImGuiKey_9;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_B: return ImGuiKey_B;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_D: return ImGuiKey_D;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_F: return ImGuiKey_F;
    case SDLK_G: return ImGuiKey_G;
    case SDLK_H: return ImGuiKey_H;
    case SDLK_I: return ImGuiKey_I;
    case SDLK_J: return ImGuiKey_J;
    case SDLK_K: return ImGuiKey_K;
    case SDLK_L: return ImGuiKey_L;
    case SDLK_M: return ImGuiKey_M;
    case SDLK_N: return ImGuiKey_N;
    case SDLK_O: return ImGuiKey_O;
    case SDLK_P: return ImGuiKey_P;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_R: return ImGuiKey_R;
    case SDLK_S: return ImGuiKey_S;
    case SDLK_T: return ImGuiKey_T;
    case SDLK_U: return ImGuiKey_U;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_W: return ImGuiKey_W;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    case SDLK_F1: return ImGuiKey_F1;
    case SDLK_F2: return ImGuiKey_F2;
    case SDLK_F3: return ImGuiKey_F3;
    case SDLK_F4: return ImGuiKey_F4;
    case SDLK_F5: return ImGuiKey_F5;
    case SDLK_F6: return ImGuiKey_F6;
    case SDLK_F7: return ImGuiKey_F7;
    case SDLK_F8: return ImGuiKey_F8;
    case SDLK_F9: return ImGuiKey_F9;
    case SDLK_F10: return ImGuiKey_F10;
    case SDLK_F11: return ImGuiKey_F11;
    case SDLK_F12: return ImGuiKey_F12;
    case SDLK_F13: return ImGuiKey_F13;
    case SDLK_F14: return ImGuiKey_F14;
    case SDLK_F15: return ImGuiKey_F15;
    case SDLK_F16: return ImGuiKey_F16;
    case SDLK_F17: return ImGuiKey_F17;
    case SDLK_F18: return ImGuiKey_F18;
    case SDLK_F19: return ImGuiKey_F19;
    case SDLK_F20: return ImGuiKey_F20;
    case SDLK_F21: return ImGuiKey_F21;
    case SDLK_F22: return ImGuiKey_F22;
    case SDLK_F23: return ImGuiKey_F23;
    case SDLK_F24: return ImGuiKey_F24;
    case SDLK_AC_BACK: return ImGuiKey_AppBack;
    case SDLK_AC_FORWARD: return ImGuiKey_AppForward;
    default: break;
    }

    switch (scancode) {
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_NONUSBACKSLASH: return ImGuiKey_Oem102;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    default: break;
    }
    return ImGuiKey_None;
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

std::optional<WindowsSettingsDisplay>
windowsSettingsDisplayByNumber(int settingsNumber) {
    const auto displays = windowsSettingsDisplays();
    const auto it = std::ranges::find_if(
        displays,
        [settingsNumber](const WindowsSettingsDisplay& display) {
            return display.settingsNumber == settingsNumber;
        });
    if (it == displays.end()) {
        return std::nullopt;
    }
    return *it;
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

#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    if (windows_.empty()) {
        rawWindowId_ = static_cast<std::uint32_t>(
            SDL_GetWindowID(window));
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
    int displayNumberOverride) {
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
    if (displayNumberOverride > 0 ||
        logicalRole < roleToWindowsSettingsDisplay.size()) {
        const int settingsNumber = displayNumberOverride > 0
            ? displayNumberOverride
            : roleToWindowsSettingsDisplay[logicalRole];
        const auto display =
            windowsSettingsDisplayByNumber(settingsNumber);
        if (!display.has_value()) {
            return false;
        }

        return placeBorderlessWindow(
            window,
            display->x,
            display->y,
            display->width,
            display->height);
    }
#endif

    // Portable fallback: use SDL enumeration order. This path is only for
    // non-Windows platforms; Windows uses CCD path numbering above so the
    // operator-facing Settings numbers are authoritative.
    return placeWindowFullscreenOnDisplay(
        window, displayNumberOverride > 0
            ? static_cast<std::size_t>(displayNumberOverride - 1)
            : logicalRole);
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
                updateRemoteModifierKeys(
                    remoteUiInput_,
                    event.key.mod);
                if (!isRegLocalHotkey(event.key.key)) {
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
