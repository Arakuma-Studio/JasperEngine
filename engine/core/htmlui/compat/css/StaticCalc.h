// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_STATICCALC_H
#define COMPAT_CSS_STATICCALC_H

// Static evaluation of CSS math functions (phase-2 §3.9.6 step 4): calc(), min(), max(),
// clamp() while the fork lacks Calc, and round(), mod(), rem(), abs(), sign(), the
// trigonometric functions, pow(), sqrt(), hypot(), log(), exp() always (RmlUi has none of
// them). A value is a sum of terms per unit; absolute lengths, angles, times and resolutions
// fold exactly into their canonical unit (px, deg, s, x), relative units stay symbolic until
// the caller resolves them with the load-time context.

#include "Export.h"
#include "htmlui/compat/css/CssAst.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct CalcValue {
        // unit -> coefficient: "" a number, "%" a percentage, canonical units ("px", "deg",
        // "s", "x", "hz") and relative units as written in lowercase ("em", "vw", "fr", ...)
        std::vector<std::pair<std::string, double>> terms;

        static CalcValue number(double value);
        static CalcValue of(double value, std::string unit);

        bool isZero() const;
        bool isNumber() const;                    // only a number term (or nothing)
        bool single() const { return terms.size() == 1; }
        double numberValue() const;               // the "" term
        double get(std::string_view unit) const;  // 0 when absent
        bool has(std::string_view unit) const;
        void add(const std::string& unit, double value);
        CalcValue plus(const CalcValue& other, double sign = 1.0) const;
        CalcValue scaled(double factor) const;
    };

    struct CalcContext {
        // Identifiers allowed inside the expression besides e/pi/infinity/nan (the channel
        // keywords of a relative color: "r", "h", "alpha", ...)
        std::function<std::optional<double>(std::string_view identifier)> identifier;
        // A var() function's static replacement; false when the variable is not static
        std::function<bool(const ComponentValue& var, std::vector<ComponentValue>& replacement)> resolveVar;
        // px per relative unit (em, rem, vw, vh, %, ...) to compare or reduce incomparable
        // terms with the load-time context; nullopt = unknown (the expression cannot fold)
        std::function<std::optional<double>(std::string_view unit)> pxPerUnit;

        // filled by the evaluation
        bool usedContext = false;        // pxPerUnit decided a comparison
        bool usedViewport = false;       // ... with a viewport unit
        bool varUnresolved = false;      // a var() had no static value
        std::vector<std::string> functions;   // the math functions met (lowercase)
    };

    class JASPERENGINE_API StaticCalc {
    public:
        // calc min max clamp round mod rem abs sign sin cos tan asin acos atan atan2 pow sqrt
        // hypot log exp (case-insensitive)
        static bool isMathFunction(std::string_view name);
        // the ones the fork's Calc feature covers: calc min max clamp
        static bool isCalcFamily(std::string_view name);

        // Evaluates a number, dimension, percentage, math function or parenthesized block.
        // nullopt with `error` set when the expression is invalid, mixes incompatible types,
        // or needs a context it does not have.
        static std::optional<CalcValue> evaluate(const ComponentValue& value, CalcContext& context, std::string* error = nullptr);
        // The sum of a list of component values (the inside of calc()).
        static std::optional<CalcValue> evaluateSum(const std::vector<ComponentValue>& values, CalcContext& context,
                                                    std::string* error = nullptr);

        // Resolves every relative term with context.pxPerUnit into px (numbers stay); nullopt
        // when a unit is unknown to the context.
        static std::optional<CalcValue> resolveToPx(const CalcValue& value, CalcContext& context);
    };

}

#endif //COMPAT_CSS_STATICCALC_H
