// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_VALUENORMALIZER_H
#define COMPAT_CSS_VALUENORMALIZER_H

// Step 4 of the declaration pipeline (phase-2 §3.9.6): every value token tree outside url()
// and strings is brought to what RCSS reads. Colors become #rrggbbaa, currentColor the
// --html-current-color companion, times seconds, angles degrees, lengths the units RmlUi
// knows; math functions are folded while the fork lacks Calc; env() and attr() are resolved
// or dropped.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/css/CssAst.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    // what a percentage of the property refers to (calc() reduction, §3.9.6 step 4)
    enum class PercentBasis : uint8_t { ContainingBlock, FontSize, LineHeight, Number, None };

    struct NormalizeContext {
        bool customProperty = false;
        bool colors = true;                   // color keywords are colors here (not in font-family, animation names, ...)
        bool keepCurrentColorKeyword = false; // SVG paint: `currentcolor` stays a keyword
        std::string staticColor;              // the rule's own static color (#rrggbbaa): currentColor uses it
        // when set: the rule's static color, asked at the first currentColor the value holds
        std::function<std::string()> ruleStaticColor;
        const std::string& color() const {
            if (ruleStaticColor && !colorAsked){
                colorAsked = true;
                colorValue = ruleStaticColor();
            }
            return ruleStaticColor ? colorValue : staticColor;
        }
        mutable bool colorAsked = false;
        mutable std::string colorValue;
        bool inlineStyle = false;             // vmin/vmax evaluated with the viewport
        // the value is written where the fork evaluates calc(), min(), max() and clamp() itself
        // (an RmlUi property whose parser takes them, a custom property RmlUi substitutes): with
        // ForkFeature::Calc they are checked and kept; otherwise (compat decorator arguments,
        // values a rewrite reads token by token) they are folded as without the feature
        bool calcNative = false;
        // attr() is kept: 'content' with the fork's PseudoElements (the originating element's attribute)
        bool attrNative = false;
        PercentBasis percentBasis = PercentBasis::ContainingBlock;
        bool vertical = false;                // the percentage basis is a height (top, bottom, height, ...)
        // the session's options (the defaults when not given)
        const CompatOptions* options = nullptr;
        const CompatOptions& compat() const {
            static const CompatOptions defaults;
            return options ? *options : defaults;
        }
        double emPx = 16.0;                   // the rule's static font-size
        // when set: the rule's static font-size (emPx when it has none), asked at the first
        // font-relative unit the value resolves: most values resolve none
        std::function<std::optional<double>()> ruleFontSize;
        double em() const {
            if (!ruleFontSize)
                return emPx;
            if (!emAsked){
                emAsked = true;
                const std::optional<double> px = ruleFontSize();
                emValue = px ? *px : emPx;
            }
            return emValue;
        }
        mutable bool emAsked = false;
        mutable double emValue = 16.0;
        double remPx = 16.0;                  // the root font-size
        // a var()'s static value (custom property index)
        std::function<bool(const ComponentValue& var, ValueList& out)> resolveVar;
        // diagnostics: severity is the code's unless the code is "by case"
        std::function<void(DiagCode code, Severity severity, const std::string& message, const std::string& suggestion)> report;
    };

    struct NormalizeResult {
        ValueList value;
        bool ok = true;                       // false: drop the declaration (already reported)
        bool vmin = false;                    // vmin/vmax left for the orientation duplication
        bool dependsOnViewport = false;
        bool currentColor = false;            // currentColor became the companion
    };

    // the only non-whitespace component of a value (null when there are none or several)
    JASPERENGINE_API const ComponentValue* soleValue(const ValueList& values);
    // trimWhitespace without the copy
    JASPERENGINE_API void trimInPlace(ValueList& values);

    class JASPERENGINE_API ValueNormalizer {
    public:
        static NormalizeResult normalize(const ValueList& value, const NormalizeContext& context);
        // the same; the result's value is built in `storage` (its contents are dropped, its memory reused)
        static NormalizeResult normalize(const ValueList& value, const NormalizeContext& context,
                                         ValueList&& storage);

        // the percentage basis and orientation of a property
        static PercentBasis percentBasisOf(const std::string& property, bool* vertical = nullptr);
        // properties whose idents are not colors (font-family, animation-name, ...)
        static bool colorKeywordsApply(const std::string& property);

        // synthetic component values
        static ComponentValue ident(const std::string& name);
        static ComponentValue hash(const std::string& hexDigits);      // without '#'
        static ComponentValue number(double value);
        static ComponentValue percentage(double value);
        static ComponentValue dimension(double value, const std::string& unit);
        static ComponentValue string(const std::string& text);
        static ComponentValue whitespace();
        static ComponentValue comma();
        static ComponentValue delim(char c);
        static ComponentValue function(const std::string& name, ValueList children);
        // "var(--html-current-color, #000000ff)"
        static ComponentValue currentColorCompanion();
    };

}

#endif //COMPAT_CSS_VALUENORMALIZER_H
