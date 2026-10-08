// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_FOCUSTRACKER_H
#define COMPAT_RUNTIME_FOCUSTRACKER_H

#include "Export.h"

namespace Rml {
    class Context;
    class Element;
}

namespace jasperengine::htmlui::compat {

    // :html-focus / :html-focus-visible (phase-2 §3.8, §3.11.4). RmlUi sets :focus and
    // :focus-visible on the whole chain from the focused element to the document; compat
    // rewrites author :focus/:focus-visible to these two, which only the focused element
    // itself gets (never a document): :html-focus always, :html-focus-visible when RmlUi
    // marked the focus visible (keyboard navigation, text fields), as browsers decide
    // :focus-visible. RmlUi's chain keeps serving :focus-within.
    //
    // update() runs for every context right before Context::Update (updateRuntime): the
    // focused element and its focus-visible state are compared with the last ones (a pointer
    // and a flag) and the pseudo-classes moved when they differ. Elements are held by
    // observer pointers, so a destroyed element or context is never touched.
    class JASPERENGINE_INTERNAL FocusTracker {
    public:
        static constexpr const char* kFocus = "html-focus";
        static constexpr const char* kFocusVisible = "html-focus-visible";

        static void update(Rml::Context* context);
        // the element holding :html-focus in a context (nullptr when none)
        static Rml::Element* focused(Rml::Context* context);
        static void reset();   // releaseRuntime: forgets every context
    };

}

#endif //COMPAT_RUNTIME_FOCUSTRACKER_H
