#pragma once

#include "media/GtAccessUnit.hpp"
#include "media/GtMapWorker.hpp"

#include <cstdint>

namespace reg::media {

struct GtDisplaySelection {
    bool sky{};
    bool depth{};
};

// Presentation is independent from parsing: a decoded map belongs to a
// particular RGF1 (epoch, frame ID, source time) and visible video raster.
// Never extrapolate/reuse the last sky/depth texture on a later video frame.
// The two layers can independently fail or be unavailable.
inline GtDisplaySelection selectGtDisplayLayers(
    const FrameGtParts& frame,
    const GtDecodedBundle& decoded,
    int visibleWidth, int visibleHeight,
    bool wantSky, bool wantDepth) noexcept {
    if(!frame.source || visibleWidth<=0 || visibleHeight<=0 ||
       frame.source->key!=decoded.identity.key ||
       frame.source->sourceTimeNs!=decoded.identity.sourceTimeNs)
        return {};
    const auto matches=[&](const GtMapMetadata& header,
                           const GtDecodedMap& map,GtMapKind kind) {
        if(header.kind!=kind || map.kind!=kind ||
           header.status!=GtMapStatus::Encoded ||
           !header.sourceValid || header.source.key!=frame.source->key ||
           header.source.sourceTimeNs!=frame.source->sourceTimeNs ||
           map.status!=GtMapStatus::Ready || !map.raw ||
           map.width==0U || map.height==0U ||
           header.width!=map.width || header.height!=map.height)
            return false;
        if(header.sourceWidth!=static_cast<std::uint32_t>(visibleWidth) ||
           header.sourceHeight!=static_cast<std::uint32_t>(visibleHeight))
            return false;
        return true;
    };
    return {
        wantSky && matches(frame.sky,decoded.sky,GtMapKind::Sky),
        wantDepth && matches(frame.depth,decoded.depth,GtMapKind::Depth),
    };
}

} // namespace reg::media
