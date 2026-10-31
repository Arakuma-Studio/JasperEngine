// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLTEXTEFFECTS_H
#define HTMLTEXTEFFECTS_H

#include "Export.h"
#include "util/TextEffects.h"

#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace Rml {
    class Context;
    class Element;
}

namespace jasperengine::htmlui {

    // A parsed `text-effect` value: the layers of the shared per-glyph effects (util/TextEffects.h)
    struct JASPERENGINE_API HtmlTextEffect {
        std::vector<TextEffectLayer> layers;
        // the layer's amplitude was a length in px (divided by the text's font size when applied)
        std::vector<bool> amplitudeInPx;
    };

    // What the font engine reads for one text while it generates its glyphs
    // (Rml::TextShapingContext::text_effect)
    struct JASPERENGINE_API HtmlTextEffectGlyphs {
        const HtmlTextEffect* effect = nullptr;
        int count = 0;            // the visible characters the effect animates (its N)
        int characters = 0;       // the same with white space (TMP's characterCount: its timing)
        float time = 0.0f;        // seconds since it started (looping layers)
        float cycleTime = 0.0f;   // seconds into the one-shot cycle
        float em = 16.0f;         // the text's font size
        bool reducedMotion = false;
    };

    // `text-effect: <function>[, <function>]*`: wave(<length> <time> <angle>), bounce(<length> <time>
    // <spread>), shake(<length> <Hz>), pulse(<number> <time> <spread>), typewriter(<cps> <fade time>),
    // fade-in(<time> <spread>), grow(<time> <spread> <amplitude>), unfold(<time> <spread> <amplitude>), rainbow(<time> <hue step
    // angle> <saturation %> <lightness %>), gradient(<time> [multiply|add|replace]? <color> [<position %>]#),
    // `none`. Omitted trailing arguments take their defaults. False with `error` otherwise.
    JASPERENGINE_API bool parseTextEffect(const std::string& value, HtmlTextEffect& out, std::string& error);
    // text-effect-iteration: infinite (-1) | <integer >= 1>
    JASPERENGINE_API bool parseTextEffectIteration(const std::string& value, int& iterations);

    // The per-character text animation of the HTML UI: the fork's text effect provider
    // (ElementText::SetTextEffectProvider) and the per-frame driver that regenerates the
    // geometry of animating texts (never their layout) and dispatches `texteffectend`.
    class JASPERENGINE_API HtmlTextEffects : public Rml::TextEffectProvider {
    public:
        static HtmlTextEffects& get();

        // installs the provider (HtmlUI::initialise), removes it (shutdown)
        static void install();
        static void uninstall();

        const void* GetTextEffect(Rml::ElementText& text, int& first_index) override;
        // text-effect, text-effect-iteration or text-effect-time changed (set at runtime too)
        bool IsTextEffectChange(const Rml::PropertyIdSet& changed) override;

        // once per frame of a context, before its update: the geometry of the texts that
        // animate is generated again, finished one-shot effects send texteffectend
        void update(Rml::Context* context);
        // prefers-reduced-motion of a context: the motion functions hold still
        void setReducedMotion(Rml::Context* context, bool reduced);
        // el:restartTextEffect(): the effect of `element` (or the one its text inherits) from 0
        void restart(Rml::Element* element);
        // the context is gone: its state goes with it
        void forget(Rml::Context* context);

        // tests: the effect that applies to `text` and the state its glyphs got last
        const HtmlTextEffectGlyphs* lastGlyphs() const { return &scratch; }

    private:
        struct Scope {
            Rml::ObserverPtr<Rml::Element> root;
            Rml::Context* context = nullptr;
            std::string value;
            HtmlTextEffect effect;
            int iterations = -1;     // -1 infinite
            bool gameTime = false;
            double start = 0.0;
            double cycleStart = 0.0;
            int iterationsDone = 0;
            bool finished = false;
            // the visible characters of each inline formatting context it animates, by the
            // context's first text: a one-shot ends when the longest one did
            std::unordered_map<const Rml::ElementText*, int> counts;
            // what each of its texts showed: a change starts it again, and so does a text
            // joining in a later frame (new content: the old texts are gone)
            std::unordered_map<const Rml::ElementText*, std::string> texts;
            uint64_t textsFrame = 0;
        };

        std::unordered_map<Rml::Element*, Scope> scopes;
        std::unordered_map<Rml::Context*, bool> reducedMotion;
        std::vector<Rml::ObserverPtr<Rml::Element>> animated;
        HtmlTextEffectGlyphs scratch;
        uint64_t frame = 0;

        Rml::Element* findRoot(Rml::Element* text) const;
        double now(const Scope& scope) const;
        bool animates(const Scope& scope) const;
    };

}

#endif // HTMLTEXTEFFECTS_H
