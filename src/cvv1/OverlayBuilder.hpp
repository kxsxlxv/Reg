#pragma once

#include "cvv1/Cvv1.hpp"
#include "render/OverlayPrimitives.hpp"
#include "video/VideoTransform.hpp"

namespace reg::cvv1 {

struct Layers {
    bool observations{true};
    bool candidates{true};
    bool selectedTarget{true};
    bool status{true};
};

class OverlayBuilder {
public:
    render::OverlayScene build(const Snapshot& snapshot,
                               const video::VideoTransform& transform,
                               Layers layers = {}, bool stale = false) const;
    render::OverlayScene noSnapshot(const video::VideoTransform& transform,
                                    Layers layers, bool stale) const;
};
} // namespace reg::cvv1
