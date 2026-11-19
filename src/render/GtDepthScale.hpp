#pragma once

#include <algorithm>
#include <cmath>

namespace reg::render {

// Mirrors the normalisation in shaders/gt_map.frag. Both the colour legend
// and shader use a fixed metre range, never per-frame extrema.
inline float gtDepthLogPosition(float z,float nearM,float farM) noexcept {
    if(!std::isfinite(z) || z<=0.0F)return -1.0F;
    const float a=std::max(nearM,0.01F);
    const float b=std::max(farM,a+0.01F);
    return std::clamp(std::log(std::max(z,a)/a) /
                      std::max(std::log(b/a),1.0e-6F),0.0F,1.0F);
}

inline float gtDepthLogMidpoint(float nearM,float farM) noexcept {
    const float a=std::max(nearM,0.01F);
    return std::sqrt(a*std::max(farM,a+0.01F));
}

} // namespace reg::render
