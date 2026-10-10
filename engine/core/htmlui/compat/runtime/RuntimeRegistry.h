// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_RUNTIMEREGISTRY_H
#define COMPAT_RUNTIME_RUNTIMEREGISTRY_H

// The compat layer's runtime emulations in RmlUi (phase-2 §3.11): registered properties,
// element instancers, decorators, focus tracking, animation fill modes, list markers. This
// header is a seam: p2-core declares the entry points and wires the call sites (hook H1:
// HtmlUI::acquire/release, HtmlUIContext::update); p2-runtime owns RuntimeRegistry.cpp.

namespace Rml {
    class Context;
}

namespace jasperengine::htmlui::compat {

    // Once per RmlUi initialisation, right after Rml::Initialise() and before any document or
    // stylesheet is loaded (HtmlUI::acquire). Skips each emulation whose fork feature is
    // present (ForkFeatures::present(), §3.6). Idempotent while RmlUi stays initialised.
    void registerRuntime();
    // After Rml::Shutdown() (HtmlUI::release): drops what registerRuntime made (instancers
    // RmlUi does not own), so the next registerRuntime starts clean.
    void releaseRuntime();
    // Every frame, for every context, right before Rml::Context::Update (HtmlUIContext::update):
    // FocusTracker::update, AnimationFill::update.
    void updateRuntime(Rml::Context* context);

}

#endif //COMPAT_RUNTIME_RUNTIMEREGISTRY_H
