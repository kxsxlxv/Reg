#pragma once

#include <cstddef>
#include <cstdint>

struct SDL_Window;

namespace reg::platform {

struct DisplayPlacement {
    std::size_t displayIndex{};
    bool fullscreen{false};
};

struct DisplayPlacementResult {
    std::uint32_t requestedDisplayId{};
    std::uint32_t actualDisplayId{};
    bool usedFallback{};
};

class DisplayLayout final {
public:
    DisplayPlacementResult apply(
        SDL_Window* window,
        DisplayPlacement placement) const;

    std::size_t displayCount() const;
};

} // namespace reg::platform
