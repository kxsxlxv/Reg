#pragma once

#include <atomic>
#include <cstdint>

namespace reg::remote {

// Remote text focus is tri-stated because older NetImgui clients do not
// advertise ImGuiIO::WantTextInput. Unknown focus must fail closed: a physical
// T key must not change GT while the user might be editing remote text.
enum class RemoteTextFocus : std::uint8_t {
    Disconnected,
    Unknown,
    NotEditing,
    Editing,
};

constexpr bool gtHotkeyAllowed(RemoteTextFocus state) noexcept {
    return state == RemoteTextFocus::Disconnected ||
           state == RemoteTextFocus::NotEditing;
}

// The receive/render loop publishes focus here; SDL reads it on the same main
// loop. Atomic storage avoids data races if those loops are split later.
inline std::atomic<RemoteTextFocus> gRemoteTextFocus{
    RemoteTextFocus::Disconnected};

} // namespace reg::remote
