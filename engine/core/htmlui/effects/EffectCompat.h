// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTCOMPAT_H
#define HTMLUI_EFFECTS_EFFECTCOMPAT_H

// effect() and @effect in the compatibility layer (phase-5 spec 3.5): the handlers of the
// compat function registry (css/props/Rewriters.h) that turn an effect() image layer or filter
// function into its typed RCSS form, jfx-<name>(v(...) ...), with every diagnostic of 3.13 at
// its line and column, and declare the project effects of @effect.

#include "Export.h"

#include <optional>
#include <string>

namespace jasperengine::htmlui {

    namespace compat {
        struct ImageLayerContext;
        struct FilterContext;
        struct Session;
        struct Rule;
        class DiagnosticSink;
    }

    // registers effect() (image layers, filter lists) and @effect with the compatibility layer;
    // idempotent (the compat registry calls it on first use, EffectRegistry::initialise too)
    JASPERENGINE_API void registerEffectCompatHandlers();

    class JASPERENGINE_API EffectCompat {
    public:
        // an effect() image layer: its typed RCSS (with the paint area keyword unless the layer
        // is native), its filter form for gradient text; nullopt when dropped (reported)
        static std::optional<std::string> imageLayer(const compat::ImageLayerContext& context, compat::DiagnosticSink& sink);
        // an effect() of a filter or backdrop-filter list
        static std::optional<std::string> filterFunction(const compat::FilterContext& context, compat::DiagnosticSink& sink);
        // @effect <name> { src: url(...) }
        static void declareEffect(const compat::Rule& rule, compat::Session& session, compat::DiagnosticSink& sink);
        // the effect named by a paint() image function, for phase 2's suggestion ("" when none)
        static std::string paintSuggestion(const std::string& paintName);
    };

}

#endif //HTMLUI_EFFECTS_EFFECTCOMPAT_H
