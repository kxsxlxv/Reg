#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace reg::remote {

inline constexpr std::int32_t kRemoteUiMouseUnavailable = -30000;
inline constexpr std::size_t kRemoteUiKeyCount =
    static_cast<std::size_t>(ImGuiKey_NamedKey_COUNT);
inline constexpr std::size_t kRemoteUiKeyMaskWordCount =
    (kRemoteUiKeyCount + 63U) / 64U;
inline constexpr std::size_t kRemoteUiTextCapacity = 256U;

struct RemoteUiInputState {
    std::int32_t mouseX{kRemoteUiMouseUnavailable};
    std::int32_t mouseY{kRemoteUiMouseUnavailable};
    float wheelX{};
    float wheelY{};
    std::uint64_t mouseDownMask{};
    std::array<std::uint64_t, kRemoteUiKeyMaskWordCount> keyDownMask{};
    std::array<std::uint16_t, kRemoteUiTextCapacity> text{};
    std::uint16_t textCount{};
};

inline void setRemoteUiKey(
    RemoteUiInputState& state,
    ImGuiKey key,
    bool down) noexcept {
    const int index =
        static_cast<int>(key) -
        static_cast<int>(ImGuiKey_NamedKey_BEGIN);
    if (index < 0 ||
        index >= static_cast<int>(ImGuiKey_NamedKey_COUNT)) {
        return;
    }

    const auto word = static_cast<std::size_t>(index) / 64U;
    const auto bit = static_cast<unsigned>(index) % 64U;
    const std::uint64_t mask = std::uint64_t{1} << bit;
    if (down) {
        state.keyDownMask[word] |= mask;
    } else {
        state.keyDownMask[word] &= ~mask;
    }
}

inline void clearRemoteUiTransient(
    RemoteUiInputState& state) noexcept {
    state.wheelX = 0.0F;
    state.wheelY = 0.0F;
    state.textCount = 0U;
}

inline void clearRemoteUiPressedState(
    RemoteUiInputState& state) noexcept {
    state.mouseDownMask = 0U;
    state.keyDownMask.fill(0U);
}

inline void mergeRemoteUiInput(
    RemoteUiInputState& destination,
    const RemoteUiInputState& source) noexcept {
    destination.mouseX = source.mouseX;
    destination.mouseY = source.mouseY;
    destination.mouseDownMask = source.mouseDownMask;
    destination.keyDownMask = source.keyDownMask;
    destination.wheelX += source.wheelX;
    destination.wheelY += source.wheelY;

    const std::size_t destinationCount = destination.textCount;
    const std::size_t sourceCount = source.textCount;
    const std::size_t available =
        kRemoteUiTextCapacity - destinationCount;
    const std::size_t copyCount =
        sourceCount < available ? sourceCount : available;
    for (std::size_t index = 0; index < copyCount; ++index) {
        destination.text[destinationCount + index] = source.text[index];
    }
    destination.textCount = static_cast<std::uint16_t>(
        destinationCount + copyCount);
}

// SDL event collection and NetImgui packet generation both run on Reg's main
// thread today, but keep the hand-off synchronized so this remains safe if the
// input pump moves to a dedicated thread later.
inline std::mutex gRemoteUiInputMutex;
inline RemoteUiInputState gRemoteUiInput{};

inline void publishRemoteUiInput(
    const RemoteUiInputState& state) noexcept {
    std::scoped_lock lock(gRemoteUiInputMutex);
    mergeRemoteUiInput(gRemoteUiInput, state);
}

inline RemoteUiInputState consumeRemoteUiInput() noexcept {
    std::scoped_lock lock(gRemoteUiInputMutex);
    RemoteUiInputState result = gRemoteUiInput;
    clearRemoteUiTransient(gRemoteUiInput);
    return result;
}

} // namespace reg::remote
