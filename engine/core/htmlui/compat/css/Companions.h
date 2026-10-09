// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_COMPANIONS_H
#define COMPAT_CSS_COMPANIONS_H

// Companions (phase-2 §3.9.8): custom properties the rewriter writes next to real
// declarations so that values from different rules combine the way CSS combines them
// (currentColor, outline + box-shadow, background + border-image layers, text-shadow +
// text-stroke font effects, `inherit` on non-inherited properties, accent and control looks).

#include <string>

namespace jasperengine::htmlui::compat {

    struct DeclContext;

    namespace companions {
        constexpr const char* kCurrentColor = "--html-current-color";
        constexpr const char* kBoxShadow = "--html-box-shadow";
        constexpr const char* kOutlineShadow = "--html-outline-shadow";
        constexpr const char* kBgLayers = "--html-bg-layers";
        constexpr const char* kBorderImage = "--html-border-image";
        constexpr const char* kTextShadow = "--html-text-shadow";
        constexpr const char* kTextStroke = "--html-text-stroke";
        constexpr const char* kAccent = "--html-accent";
        constexpr const char* kUnset = "--html-unset";
        constexpr const char* kNoShadow = "0 0 0 0 #0000";
        constexpr const char* kNoLayer = "linear-gradient(#0000, #0000)";
        constexpr const char* kNoEffect = "shadow(0px 0px #0000)";

        // "var(--html-outline-shadow, 0 0 0 0 #0000), var(--html-box-shadow, 0 0 0 0 #0000)"
        std::string boxShadowList();
        // the decorator list of documents that use border-image
        std::string decoratorList();
        // the font-effect list of documents that use text-shadow and -webkit-text-stroke
        std::string fontEffectList();
        // "--html-inh-<property>"
        std::string inheritName(const std::string& rcssProperty);

        // writes a shadow ring / layers / effect value through its companion (or directly in
        // keyframe mode and where the document does not need the companion)
        void emitBoxShadow(DeclContext& context, const std::string& value);
        void emitOutline(DeclContext& context, const std::string& ring);
        void emitBackgroundLayers(DeclContext& context, const std::string& layers);   // "" = none
        void emitBorderImage(DeclContext& context, const std::string& decorator);
        void emitTextShadow(DeclContext& context, const std::string& effects);         // "" = none
        void emitTextStroke(DeclContext& context, const std::string& effect);          // "" = none
    }

}

#endif //COMPAT_CSS_COMPANIONS_H
