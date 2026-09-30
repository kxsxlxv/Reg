#pragma once

namespace reg::render {

// Google Material Symbols Outlined glyphs used by Reg. The icon font is merged
// into the default Roboto atlas so these UTF-8 strings can be used inline.
inline constexpr char kIconWarning[] = "\xEE\x80\x82";     // warning U+E002
inline constexpr char kIconVideoOff[] = "\xEE\x81\x8C";    // videocam_off U+E04C
inline constexpr char kIconCheckCircle[] = "\xEF\x82\xBE"; // check_circle U+F0BE

// Configures the current ImGui context. Roboto is the default text font and
// Material Symbols Outlined is merged into the same atlas. The palette and
// geometry intentionally mirror the operator UI style used by AtomGCS.
void configureRegImGuiTheme(float contentScale);

} // namespace reg::render
