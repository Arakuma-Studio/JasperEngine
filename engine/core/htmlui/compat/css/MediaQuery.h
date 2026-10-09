// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_MEDIAQUERY_H
#define COMPAT_CSS_MEDIAQUERY_H

// CSS media queries (Level 4) to RmlUi's @media blocks (phase-2 §3.9.4). RmlUi 6.3 takes one
// conjunction of features per block (width/height/aspect-ratio/resolution with min-/max-,
// orientation, theme) with an optional leading `not` over the whole block; no media types,
// lists, ranges or nesting, and `em` resolves against 12px. A CSS query list becomes a
// disjunction of such blocks (each holding a copy of the rules); static features (hover,
// pointer, color, ...) are evaluated at load.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/RcssWriter.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;

    // One RmlUi media block: features joined by `and`.
    struct MediaConjunction {
        struct Range {
            std::optional<double> min, max;   // inclusive
        };
        Range width, height, resolution;      // px, px, x
        Range aspect;                         // width / height
        std::string orientation;              // "", "landscape", "portrait"
        std::string theme;                    // "" or the one theme that must be active
        std::string negatedTheme;             // "" or the one theme that must not be active (block written as "not")
        bool contradiction = false;           // never matches
        bool inexpressible = false;           // RmlUi cannot express it (two themes, a negated theme with others)

        bool empty() const;                   // always true
        // the RCSS block condition
        RcssMediaCondition toRcss() const;
        // a AND b
        static MediaConjunction conjoin(const MediaConjunction& a, const MediaConjunction& b);
    };

    // A normalized query list: the disjunction of its blocks. always = matches everywhere (no
    // @media block needed); never = matches nowhere (the rules are dropped).
    struct MediaResult {
        bool always = false;
        bool never = false;
        std::vector<MediaConjunction> blocks;

        static MediaResult unconditional();
        // a AND b (nested @media, link media + @media): the cross product of the blocks
        static MediaResult conjoin(const MediaResult& a, const MediaResult& b);
    };

    class JASPERENGINE_API MediaQuery {
    public:
        // Normalizes a media query list (an @media prelude, a <link media>, an @import
        // condition). Diagnostics at ref: HTMLUI-AT-0004 (cannot be expressed / unknown
        // feature: never matches), HTMLUI-AT-0005 (rewritten, once per query), HTMLUI-AT-0006
        // (static feature with its value). sink may be null.
        static MediaResult normalize(const std::vector<ComponentValue>& prelude, SourceRef ref, const CompatOptions& options,
                                     DiagnosticSink* sink);
        static MediaResult normalize(std::string_view text, const CompatOptions& options, DiagnosticSink* sink = nullptr,
                                     SourceRef ref = SourceRef());

        // the RCSS text of each block, for tests: "(min-width: 600px)", "not (theme: dark)"
        static std::vector<std::string> blockTexts(const MediaResult& result);
    };

}

#endif //COMPAT_CSS_MEDIAQUERY_H
