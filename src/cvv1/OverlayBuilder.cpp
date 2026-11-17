#include "cvv1/OverlayBuilder.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace reg::cvv1 {
namespace {
using namespace reg::render;
constexpr Color kBlue{0.20F, 0.55F, 1.0F, 1.0F};
constexpr Color kOrange{1.0F, 180.0F/255.0F, 0.0F, 1.0F};
constexpr Color kYellow{1.0F, 1.0F, 0.0F, 1.0F};
constexpr Color kGreen{0.0F, 1.0F, 0.0F, 1.0F};
constexpr Color kWhite{1.0F, 1.0F, 1.0F, 1.0F};

void statusText(OverlayScene& scene, const video::VideoTransform& tx,
                const std::string& text, float row, Color color = kWhite) {
    const auto vp = tx.viewport();
    scene.primitives.emplace_back(TextPrimitive{
        .position = {vp.x + 8, vp.y + 8 + row * 25},
        .utf8 = text,
        .textColor = color,
        .backgroundColor = {0,0,0,0.75F},
        .fontSizePx = 17,
        .paddingPx = 3,
    });
}
void rectangle(OverlayScene& scene, const video::VideoTransform& tx, Box box,
               Color color, float thickness, bool dashed, std::string label = {}) {
    auto r = tx.normalizedToScreen({box.x, box.y, box.width, box.height});
    const auto vp = tx.viewport();
    const auto l = std::max(r.x, vp.x), t = std::max(r.y, vp.y);
    const auto right = std::min(r.x + r.width, vp.x + vp.width);
    const auto bottom = std::min(r.y + r.height, vp.y + vp.height);
    if (right <= l || bottom <= t) return;
    r = {l,t,right-l,bottom-t};
    scene.primitives.emplace_back(RectPrimitive{
        .rect = r,
        .style = {.color = color, .thickness = thickness,
                  .pattern = dashed ? LinePattern::Dashed : LinePattern::Solid}
    });
    if (label.empty()) return;
    // Keep long candidate IDs and hit histories inside the video viewport.
    const float labelWidth = static_cast<float>(label.size()) * 9.0F + 8.0F;
    const float x = std::clamp(r.x, vp.x, std::max(vp.x,vp.x+vp.width-labelWidth));
    float y = r.y - 25;
    if (y < vp.y + 35) y = r.y + r.height + 4;
    if (y + 23 > vp.y + vp.height) return;
    scene.primitives.emplace_back(TextPrimitive{
        .position = {x,y}, .utf8 = std::move(label), .textColor = color,
        .backgroundColor = {0,0,0,0.8F}, .fontSizePx = 15, .paddingPx = 3
    });
}
const char* stateName(State state) {
    switch(state) {
        case State::Search: return "SEARCH";
        case State::Confirmation: return "CONFIRMATION";
        case State::Track: return "TRACK";
        case State::Reset: return "RESET";
    }
    return "UNKNOWN";
}
} // namespace

render::OverlayScene OverlayBuilder::build(const Snapshot& snapshot,
                                          const video::VideoTransform& transform,
                                          Layers layers, bool stale) const {
    render::OverlayScene scene;
    scene.primitives.reserve(snapshot.detections.size()*2 +
                             snapshot.candidates.size()*2 + 8);
    if (layers.observations) {
        // Observations must be drawn first, behind linked candidates.
        for (const auto& d : snapshot.detections) {
            char label[96]{};
            std::snprintf(label,sizeof(label),"YOLO C%u %.2f",d.classId,
                          static_cast<double>(d.confidence));
            rectangle(scene,transform,d.bbox,kBlue,1.25F,false,label);
        }
    }
    if (layers.candidates) {
        for (const auto& c : snapshot.candidates) {
            const bool missed = !c.matchedThisFrame;
            Color color = c.confirmed ? kYellow : kOrange;
            if (missed) color.a = .52F;
            char label[192]{};
            std::snprintf(label,sizeof(label),
                "C%llu %u/%u need=%u/%u sum=%.1f miss=%u%s",
                static_cast<unsigned long long>(c.id),
                c.hitCount,c.windowLength,
                snapshot.confirmation.requiredHits,snapshot.confirmation.window,
                c.confidenceSum,c.missStreak,
                c.confirmed ? " CONFIRMED" : "");
            rectangle(scene,transform,c.bbox,color,1.5F,missed,label);
        }
    }
    if (layers.selectedTarget && snapshot.selectedTarget) {
        const auto& t = *snapshot.selectedTarget;
        const Color color = t.detectorSeed ? kOrange : kGreen;
        char label[128]{};
        std::snprintf(label,sizeof(label),"%s T%llu %.2f%s",
                      t.detectorSeed ? "SEED" : "LoRAT",
                      static_cast<unsigned long long>(t.id),
                      static_cast<double>(t.confidence),
                      t.healthy ? "" : " UNHEALTHY");
        rectangle(scene,transform,t.bbox,color,3.5F,!t.healthy,label);
    }
    if (layers.status) {
        char line[256]{};
        if (snapshot.state == State::Track && snapshot.selectedTarget) {
            std::snprintf(line,sizeof(line),
                "TRACK T%llu confidence=%.2f weak=%u/%u  %.1f FPS",
                static_cast<unsigned long long>(snapshot.selectedTarget->id),
                static_cast<double>(snapshot.confidence),
                snapshot.weakResponses,snapshot.weakLimit,
                snapshot.processingFps);
        } else {
            std::snprintf(line,sizeof(line),
                "%s candidates=%u confirmed=%u best=%u/%u  %.1f FPS",
                stateName(snapshot.state),snapshot.counts.candidates,
                snapshot.counts.confirmed,snapshot.counts.bestHits,
                snapshot.confirmation.window,snapshot.processingFps);
        }
        statusText(scene,transform,line,0,
            snapshot.state == State::Reset ? kOrange : kWhite);
        if (snapshot.confirmation.graceRemaining) {
            std::snprintf(line,sizeof(line),"selection grace %u/%u detector updates",
                          *snapshot.confirmation.graceRemaining,
                          snapshot.confirmation.selectionGraceUpdates);
            statusText(scene,transform,line,1,kYellow);
        }
        if (snapshot.lostThisFrame) statusText(scene,transform,
                                               "TARGET LOST / RESET",2,kOrange);
        if (snapshot.truncated) {
            std::snprintf(line,sizeof(line),"TRUNCATED: omitted YOLO=%u candidates=%u",
                          snapshot.omittedDetections,snapshot.omittedCandidates);
            statusText(scene,transform,line,3,kOrange);
        }
        if (stale) statusText(scene,transform,"CVV1 STALE (receiver-local)",4,kOrange);
    }
    return scene;
}

render::OverlayScene OverlayBuilder::noSnapshot(const video::VideoTransform& transform,
                                                 Layers layers, bool stale) const {
    render::OverlayScene scene;
    if (layers.status) statusText(scene,transform,
        stale ? "CVV1 STALE - no exact snapshot" : "CVV1 - waiting for exact frame",
        0, kOrange);
    return scene;
}
} // namespace reg::cvv1
