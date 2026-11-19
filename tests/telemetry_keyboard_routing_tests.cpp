#include "platform/SdlImGuiKeyMap.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* name) {
    if(!condition) throw std::runtime_error(name);
}
}

int main() {
    try {
        using reg::platform::sdlKeyToImGuiKey;
        require(sdlKeyToImGuiKey(SDLK_0,SDL_SCANCODE_0)==ImGuiKey_0,
                "top-row zero for GT numeric field");
        require(sdlKeyToImGuiKey(SDLK_7,SDL_SCANCODE_7)==ImGuiKey_7,
                "top-row decimal digit for GT numeric field");
        require(sdlKeyToImGuiKey(SDLK_UNKNOWN,SDL_SCANCODE_KP_3)==ImGuiKey_Keypad3,
                "keypad digit for GT numeric field");
        require(sdlKeyToImGuiKey(SDLK_UNKNOWN,SDL_SCANCODE_KP_PERIOD)==
                ImGuiKey_KeypadDecimal,"keypad decimal for GT numeric field");
        require(sdlKeyToImGuiKey(SDLK_PERIOD,SDL_SCANCODE_PERIOD)==ImGuiKey_Period,
                "decimal point for GT numeric field");
        require(sdlKeyToImGuiKey(SDLK_MINUS,SDL_SCANCODE_MINUS)==ImGuiKey_Minus,
                "negative numeric value for GT field");
        require(sdlKeyToImGuiKey(SDLK_BACKSPACE,SDL_SCANCODE_BACKSPACE)==
                ImGuiKey_Backspace,"GT field deletion");
        require(sdlKeyToImGuiKey(SDLK_RETURN,SDL_SCANCODE_RETURN)==ImGuiKey_Enter,
                "GT numeric edit commit");
        require(sdlKeyToImGuiKey(SDLK_ESCAPE,SDL_SCANCODE_ESCAPE)==ImGuiKey_Escape,
                "GT numeric edit cancel");
        require(sdlKeyToImGuiKey(SDLK_LEFT,SDL_SCANCODE_LEFT)==ImGuiKey_LeftArrow,
                "numeric cursor navigation");
        require(sdlKeyToImGuiKey(SDLK_TAB,SDL_SCANCODE_TAB)==ImGuiKey_Tab,
                "GT focus navigation");
        require(sdlKeyToImGuiKey(SDLK_LCTRL,SDL_SCANCODE_LCTRL)==ImGuiKey_LeftCtrl,
                "Ctrl + click input edit");
        require(sdlKeyToImGuiKey(SDLK_U,SDL_SCANCODE_U)==ImGuiKey_U,
                "Raw U key mapping unchanged");
        std::cout<<"telemetry_keyboard_routing_tests: PASS\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"telemetry_keyboard_routing_tests: FAIL: "<<e.what()<<'\n';
        return 1;
    }
}
