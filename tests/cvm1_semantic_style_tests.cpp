#include "render/Cvm1SemanticStyle.hpp"
#include <cassert>
#include <iostream>
#include <memory>
using namespace reg::render::cvm1;
int main() {
    Palette p;
    assert(p.resolve(0) == Kind::fallback);
    assert(p.resolve(kDetector) == Kind::detection);
    assert(p.resolve(kPropagated) == Kind::propagated);
    assert(p.resolve(kTentative | kDetector) == Kind::tentative);
    assert(p.resolve(kConfirmed | kPredicted) == Kind::predicted);
    assert(p.resolve(kConfirmed | kHeld) == Kind::propagated);
    assert(p.resolve(kSotSelected | kWeak | kPredicted) == Kind::sot_weak);
    assert(p.resolve(kSotSelected | kTentative) == Kind::sot_active);
    assert(p.resolve(kConfirmed | kDetector) == Kind::confirmed);
    assert(p.resolve(kPredicted | kHeld) == Kind::fallback);
    assert(p.resolve(kTentative | kConfirmed) == Kind::fallback);
    assert(p.resolve(kConfirmed | kLost) == Kind::fallback);
    assert(p.resolve(kSotSelected | kLost) == Kind::fallback);
    assert(p.resolve(kConfirmed | 0xFE00) == Kind::confirmed);
    assert(p.resolve(kDetector | 0x0200) == Kind::detection);
    p.rules.push_back({static_cast<std::uint16_t>(kConfirmed | kDetector),
                       static_cast<std::uint16_t>(kConfirmed | kDetector), Kind::sot_active});
    assert(p.resolve(kConfirmed | kDetector) == Kind::sot_active);
    assert(p.resolve(kTentative | kConfirmed | kDetector) == Kind::fallback);
    publish(std::make_shared<const Palette>(p));
    assert(current()->resolve(kConfirmed | kDetector) == Kind::sot_active);
    assert(isEphemeralId(frameLocalId(2)));
    assert(!isEphemeralId(42));
    std::cout << "CVM1 semantic rules: PASS\n";
}
