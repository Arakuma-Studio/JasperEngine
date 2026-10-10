// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_ANIMATIONFILL_H
#define COMPAT_RUNTIME_ANIMATIONFILL_H

#include "Export.h"

#include <cstddef>

namespace Rml {
    class Context;
    class Element;
}

namespace jasperengine::htmlui::compat {

    // animation-fill-mode: forwards / both (phase-2 §3.11.5), while the fork lacks
    // ForkFeature::AnimationLonghands (updateRuntime skips it when the fork has it). RmlUi removes
    // an animation's values when it ends; compat writes `-html-animation-fill: forwards` next to
    // such animations, and this keeps the end state: a capture listener on each context's root
    // element sees every `animationend` (the phase-3 fork dispatches one per animation, with
    // `animationName`; RmlUi 6.3 one per animated property, with `property`), finds the keyframes
    // of the element's animations that animate each property (StyleSheet::GetKeyframes) and sets
    // the last keyframe's value (the first one for an alternate animation with an even
    // iteration count) as an inline property. update()
    // (every frame, before Context::Update) removes those values again when the element's
    // `animation` changed or the element left its document, unless something else replaced
    // the value meanwhile. `backwards` is not emulated (HTMLUI-CSS-0021 at rewrite time).
    class JASPERENGINE_INTERNAL AnimationFill {
    public:
        static void update(Rml::Context* context);
        static void reset();                      // releaseRuntime
        static size_t filledCount();              // tests: properties held over all contexts
        static bool isFilled(Rml::Element* element);
    };

}

#endif //COMPAT_RUNTIME_ANIMATIONFILL_H
