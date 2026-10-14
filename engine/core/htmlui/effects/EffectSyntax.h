// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTSYNTAX_H
#define HTMLUI_EFFECTS_EFFECTSYNTAX_H

#include "EffectTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui {

    struct EffectArg {
        std::string name, value;               // name lower case, value trimmed (as written)
        size_t nameOffset = 0, valueOffset = 0;
        size_t length = 0;                     // the whole argument
    };

    struct EffectCall {
        std::string name;                      // lower case
        std::vector<EffectArg> args;
        size_t nameOffset = 0;
    };

    // The author syntax effect(<name>, <param> <value>, ...) (phase-5 spec 3.2) and the typed
    // RCSS form compat writes for RmlUi (3.5): jfx-<name>(v(<css>) ... ) [<paint area>], one
    // slot per manifest parameter in manifest order (a position takes vx() and vy()), then
    // opacity, time-scale, time-offset and, for a paint effect, blend. Offsets are bytes into the
    // text given to parse(); diagnostics carry them as their column (EffectDiagnostic).
    class JASPERENGINE_API EffectSyntax {
    public:
        // "effect(...)" (ASCII case-insensitive function name)
        static bool parse(std::string_view functionText, EffectCall& out, std::vector<EffectDiagnostic>& diags);
        // the arguments alone ("aurora, speed 2"), as RmlUi's fallback instancers receive them;
        // offsets from the start of `text`
        static bool parseArguments(std::string_view text, EffectCall& out, std::vector<EffectDiagnostic>& diags);
        // The typed RCSS form of a call, every parameter checked: unknown parameters
        // (effect-unknown-param), bad values (effect-bad-value, the default used), out of range
        // (effect-out-of-range, clamped), repeated (effect-duplicate-param, the last wins), blend on
        // a filter effect (effect-blend-on-filter). var() values are kept as they are.
        // paintArea: the decorator's paint area keyword ("" = none: a filter, or a native layer).
        static std::string toRcss(const EffectCall& call, const EffectManifest& m, bool filterForm, std::string_view paintArea,
                                  std::vector<EffectDiagnostic>& diags);
        // the universal parameters every effect takes (opacity, time-scale, time-offset, blend)
        static const std::vector<EffectParamDef>& universalParams();
        // the slot properties of an effect in order: a parameter's name, <p>-x/<p>-y for a
        // position, then the universal ones ("blend" only for paint effects)
        static std::vector<std::string> slotNames(const EffectManifest& m);
        // the canonical default text of a parameter
        static std::string defaultCanonical(const EffectParamDef& def);
        // "v(<text>)", or "vx(...)"/"vy(...)"
        static std::string slot(const std::string& text, char axis = 0);
        // the inside of v(...) / vx(...) / vy(...) ("" when the text is not one); axis gets 'x', 'y' or 0
        static bool unwrapSlot(std::string_view text, std::string& inner, char& axis);
        // Levenshtein distance (suggestions)
        static size_t editDistance(std::string_view a, std::string_view b);
    };

}

#endif //HTMLUI_EFFECTS_EFFECTSYNTAX_H
