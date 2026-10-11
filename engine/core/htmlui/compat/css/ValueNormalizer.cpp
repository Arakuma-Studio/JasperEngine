// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/ValueNormalizer.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/StringIndex.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/StaticCalc.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine::htmlui::compat;

ComponentValue ValueNormalizer::ident(const std::string& name){
    ComponentValue v;
    v.token.type = TokenType::Ident;
    v.token.value = name;
    return v;
}

ComponentValue ValueNormalizer::hash(const std::string& hexDigits){
    ComponentValue v;
    v.token.type = TokenType::Hash;
    v.token.value = hexDigits;
    v.token.hashIsId = false;
    return v;
}

ComponentValue ValueNormalizer::number(double value){
    ComponentValue v;
    v.token.type = TokenType::Number;
    v.token.number = value;
    v.token.isInteger = std::floor(value) == value;
    return v;
}

ComponentValue ValueNormalizer::percentage(double value){
    ComponentValue v;
    v.token.type = TokenType::Percentage;
    v.token.number = value;
    return v;
}

ComponentValue ValueNormalizer::dimension(double value, const std::string& unit){
    ComponentValue v;
    v.token.type = TokenType::Dimension;
    v.token.number = value;
    v.token.unit = unit;
    return v;
}

ComponentValue ValueNormalizer::string(const std::string& text){
    ComponentValue v;
    v.token.type = TokenType::String;
    v.token.value = text;
    return v;
}

ComponentValue ValueNormalizer::whitespace(){
    ComponentValue v;
    v.token.type = TokenType::Whitespace;
    return v;
}

ComponentValue ValueNormalizer::comma(){
    ComponentValue v;
    v.token.type = TokenType::Comma;
    return v;
}

ComponentValue ValueNormalizer::delim(char c){
    ComponentValue v;
    v.token.type = TokenType::Delim;
    v.token.value = std::string(1, c);
    return v;
}

ComponentValue ValueNormalizer::function(const std::string& name, ValueList children){
    ComponentValue v;
    v.kind = ComponentValue::Kind::Function;
    v.token.type = TokenType::Function;
    v.token.value = name;
    v.children = std::move(children);
    return v;
}

ComponentValue ValueNormalizer::currentColorCompanion(){
    return function("var", {ident("--html-current-color"), comma(), whitespace(), hash("000000ff")});
}

PercentBasis ValueNormalizer::percentBasisOf(const std::string& property, bool* vertical){
    static const StringIndex verticalProperties = makeStringSet({
        "height", "min-height", "max-height", "top", "bottom", "block-size", "min-block-size", "max-block-size",
        "inset-block", "inset-block-start", "inset-block-end", "row-gap", "background-position-y", "transform-origin-y",
        "perspective-origin-y",
    });
    if (vertical)
        *vertical = verticalProperties.contains(property);
    if (property == "font-size" || property == "font")
        return PercentBasis::FontSize;
    if (property == "line-height")
        return PercentBasis::LineHeight;
    if (property == "opacity" || property == "flex-grow" || property == "flex-shrink" || property == "z-index" ||
        property == "fill-opacity" || property == "stroke-opacity")
        return PercentBasis::Number;
    return PercentBasis::ContainingBlock;
}

bool ValueNormalizer::colorKeywordsApply(const std::string& property){
    static const StringIndex noColors = makeStringSet({
        "font-family", "font", "animation", "animation-name", "transition", "transition-property", "will-change",
        "counter-reset", "counter-increment", "counter-set", "grid-area", "grid-template-areas", "grid-row", "grid-column",
        "grid-row-start", "grid-row-end", "grid-column-start", "grid-column-end", "content", "quotes", "list-style-type",
        "font-feature-settings", "font-variation-settings", "unicode-range", "container-name", "view-transition-name",
        "anchor-name", "position-anchor", "cursor", "font-palette", "src", "-rmlui-language", "nav", "nav-up", "nav-down",
        "nav-left", "nav-right",
    });
    return !noColors.contains(property);
}

namespace {

    class Walker {
    public:
        explicit Walker(const NormalizeContext& context): context(context), keepLengthSums(context.lengthSums) {}

        NormalizeResult result;

        ValueList list(const ValueList& values){
            return list(values, ValueList());
        }

        // (out: storage to build the list in)
        ValueList list(const ValueList& values, ValueList&& storage){
            ValueList out = std::move(storage);
            out.clear();
            out.reserve(values.size());
            for (const ComponentValue& v : values){
                if (!result.ok)
                    break;
                if (v.isToken(TokenType::Comment))
                    continue;
                one(v, out);
            }
            return out;
        }

    private:
        void report(DiagCode code, Severity severity, const std::string& message, const std::string& suggestion = std::string()){
            // kept until the value is done: the sink keeps one finding per code and declaration, so
            // findings of one code (a 'ch' and an 'ex' approximated) are merged into one message
            for (Pending& pending : pendingReports){
                if (pending.code == code){
                    pending.severity = std::min(pending.severity, severity);
                    if (pending.message.find(message) == std::string::npos)
                        pending.message += "; " + message;
                    if (!suggestion.empty() && pending.suggestion.find(suggestion) == std::string::npos)
                        pending.suggestion += (pending.suggestion.empty() ? "" : "; ") + suggestion;
                    return;
                }
            }
            pendingReports.push_back(Pending{code, severity, message, suggestion});
        }

    public:
        void flushReports(){
            if (context.report){
                for (const Pending& pending : pendingReports)
                    context.report(pending.code, pending.severity, pending.message, pending.suggestion);
            }
            pendingReports.clear();
        }

    private:

        void fail(DiagCode code, const std::string& message, const std::string& suggestion = std::string()){
            report(code, Severity::Warning, message, suggestion);
            result.ok = false;
        }

        ColorContext colorContext() const {
            ColorContext c;
            c.resolveVar = context.resolveVar;
            return c;
        }

        void color(const ComponentValue& v, ValueList& out){
            // #rgb, #rgba, #rrggbb and #rrggbbaa are their own eight digits in lower case (what
            // the parser and ColorValue::hex below make of them)
            if (v.isToken(TokenType::Hash)){
                const Text& digits = v.token.value;
                const size_t size = digits.size();
                if (size == 3 || size == 4 || size == 6 || size == 8){
                    char hex[8] = {'0', '0', '0', '0', '0', '0', 'f', 'f'};
                    bool valid = true;
                    for (size_t i = 0; i < size && valid; i++){
                        char c = digits[i];
                        if (c >= 'A' && c <= 'F')
                            c = (char)(c - 'A' + 'a');
                        valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                        if (size <= 4)
                            hex[2 * i] = hex[2 * i + 1] = c;
                        else
                            hex[i] = c;
                    }
                    if (valid){
                        ComponentValue& made = out.emplace_back();
                        made.token.type = TokenType::Hash;
                        made.token.value = Text(std::string_view(hex, 8));
                        made.token.hashIsId = false;
                        return;
                    }
                }
            }
            ColorParseInfo info;
            bool clipped = false;
            std::optional<CssColor> parsed = ColorValue::parse(v, info, colorContext());
            if (info.currentColor){
                if (context.keepCurrentColorKeyword){
                    out.push_back(ValueNormalizer::ident("currentcolor"));
                }else if (!context.color().empty()){
                    out.push_back(ValueNormalizer::hash(context.color().substr(1)));
                }else{
                    out.push_back(ValueNormalizer::currentColorCompanion());
                    result.currentColor = true;
                }
                return;
            }
            if (!parsed && info.currentColorMix && !info.dynamicVar){
                if (context.currentColorMixNative && context.compat().fork.has(ForkFeature::CurrentColor)){
                    out.push_back(v);
                    return;
                }
                // elsewhere (compat's decorators and companions) currentcolor is the rule's static color
                const std::string current = context.color();
                if (current.size() == 9){
                    ColorParseInfo staticInfo;
                    ColorContext staticContext = colorContext();
                    staticContext.currentColor = current;
                    if (std::optional<CssColor> mixed = ColorValue::parse(v, staticInfo, staticContext)){
                        const uint32_t rgba = ColorValue::toRgba8(*mixed, &clipped);
                        report(codes::ApproximatedMinor, Severity::Info, "'" + serialize(v) + "' mixes the rule's color " + current.substr(0, 7) +
                               " (it does not follow a change of color here)", "use the color in a color property, or write the mixed color");
                        out.push_back(ValueNormalizer::hash(ColorValue::hex(rgba).substr(1)));
                        return;
                    }
                }
            }
            if (!parsed){
                if (info.dynamic){
                    fail(codes::ColorUnresolvable, "color '" + serialize(v) + "' depends on a value that is only known at runtime; declaration dropped",
                         "give the variable one static value, or write the color without var()");
                }else{
                    fail(codes::UnsupportedValue, "invalid color '" + serialize(v) + "'" + (info.error.empty() ? "" : " (" + info.error + ")") +
                         "; declaration dropped", "write a CSS color such as #rrggbb or rgb(r g b / a)");
                }
                return;
            }
            const uint32_t rgba = ColorValue::toRgba8(*parsed, &clipped);
            if (clipped)
                report(codes::ApproximatedMinor, Severity::Info, "color '" + serialize(v) + "' is outside sRGB and is clipped to " + ColorValue::hex(rgba));
            if (info.lightDark)
                report(codes::ApproximatedMinor, Severity::Info, "'" + serialize(v) + "' uses the light color (the engine has one color scheme per value)",
                       "switch colors with @media (prefers-color-scheme: dark)");
            out.push_back(ValueNormalizer::hash(ColorValue::hex(rgba).substr(1)));
        }

        void dimension(const ComponentValue& v, ValueList& out){
            const Token& t = v.token;
            // the units the fork computes itself (ForkFeature::ExtraUnits) stay where the value reaches
            // RmlUi as it is written; elsewhere (decorator arguments, values read token by token) they
            // are converted as without the feature
            if (context.unitsNative && context.compat().fork.has(ForkFeature::ExtraUnits) && isForkUnit(t.unit)){
                out.push_back(v);
                return;
            }
            const RcssUnit unit = rcssUnit(t.unit);
            if (unit.unsupported){
                fail(codes::UnsupportedUnit, "container query unit '" + t.unit + "' is not supported; declaration dropped",
                     "use vw / vh, or % of the parent");
                return;
            }
            if (unit.approximated && reportedApproximations.insert(lowerAscii(t.unit)).second){
                report(codes::ApproximatedMinor, Severity::Info, "unit '" + t.unit + "' is approximated as " + formatNumber(unit.factor) + unit.unit,
                       "use em or rem");
            }
            if (unit.viewportOrientation){
                if (context.inlineStyle){
                    const double vw = context.compat().viewport.x / 100.0;
                    const double vh = context.compat().viewport.y / 100.0;
                    const double perUnit = unit.unit == "vmin" ? std::min(vw, vh) : std::max(vw, vh);
                    out.push_back(ValueNormalizer::dimension(t.number * perUnit, "px"));
                    result.dependsOnViewport = true;
                    report(codes::ViewportDependent, Severity::Info, "'" + serialize(v) + "' (in a style attribute or keyframe, which has no per-orientation copy) is evaluated for the current viewport (" +
                           std::to_string(context.compat().viewport.x) + "x" + std::to_string(context.compat().viewport.y) + ")");
                    return;
                }
                result.vmin = true;
                out.push_back(ValueNormalizer::dimension(t.number, unit.unit));
                return;
            }
            // (unit.unit is lowercase)
            if (!equalsNoCase(unit.unit, t.unit) || unit.factor != 1.0){
                out.push_back(ValueNormalizer::dimension(t.number * unit.factor, unit.unit));
                return;
            }
            out.push_back(v);
        }

        std::optional<double> pxPerUnit(std::string_view unit){
            const std::string u = lowerAscii(unit);
            const double vw = context.compat().viewport.x / 100.0;
            const double vh = context.compat().viewport.y / 100.0;
            if (u == "em") return context.em();
            if (u == "rem") return context.remPx;
            if (u == "vw" || u == "vi" || u == "dvw" || u == "svw" || u == "lvw") return vw;
            if (u == "vh" || u == "vb" || u == "dvh" || u == "svh" || u == "lvh") return vh;
            if (u == "vmin") return std::min(vw, vh);
            if (u == "vmax") return std::max(vw, vh);
            if (u == "ch" || u == "ex") return context.em() * 0.5;
            if (u == "cap") return context.em() * 0.7;
            if (u == "ic") return context.em();
            if (u == "lh") return context.em() * 1.2;
            if (u == "rlh") return context.remPx * 1.2;
            if (u == "dp") return context.compat().dpRatio;
            if (u == "%"){
                // a percentage compared with lengths: its basis estimated with the viewport
                if (context.percentBasis == PercentBasis::FontSize)
                    return context.em() / 100.0;
                if (context.percentBasis == PercentBasis::LineHeight)
                    return context.em() / 100.0;
                if (context.percentBasis == PercentBasis::ContainingBlock){
                    percentEstimated = true;
                    return context.vertical ? vh : vw;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        // the folded value as component values (relative units mapped like any dimension)
        // A translate function folded without the fork's calc(): an argument that is a sum of lengths
        // (50% + 1em) is written as one translation per term, translate(50%) translate(1em), which
        // is exact; the other arguments are 0 in the added translations.
        void translate(const ComponentValue& v, ValueList& out){
            const bool saved = keepLengthSums;
            keepLengthSums = true;
            ComponentValue copy = shell(v);
            copy.children = list(v.children);
            keepLengthSums = saved;
            if (!result.ok)
                return;
            std::vector<ValueList> args = splitAtCommas(copy.children);
            std::vector<std::vector<ComponentValue>> terms(args.size());
            size_t count = 1;
            for (size_t i = 0; i < args.size(); i++){
                ValueList arg = trimWhitespace(args[i]);
                terms[i] = ValueNormalizer::lengthSumTerms(arg);
                count = std::max(count, terms[i].size());
            }
            if (count == 1){
                out.push_back(std::move(copy));
                return;
            }
            for (size_t k = 0; k < count; k++){
                if (k > 0)
                    out.push_back(ValueNormalizer::whitespace());
                ComponentValue part = shell(v);
                for (size_t i = 0; i < args.size(); i++){
                    if (i > 0){
                        part.children.push_back(ValueNormalizer::comma());
                        part.children.push_back(ValueNormalizer::whitespace());
                    }
                    if (k < terms[i].size())
                        part.children.push_back(terms[i][k]);
                    else if (k == 0){
                        for (const ComponentValue& c : trimWhitespace(args[i]))
                            part.children.push_back(c);
                    }else
                        part.children.push_back(ValueNormalizer::dimension(0, "px"));
                }
                out.push_back(std::move(part));
            }
        }

        void emitCalc(const CalcValue& value, ValueList& out){
            ValueList raw;
            if (value.terms.empty()){
                raw.push_back(ValueNormalizer::number(0));
            }else{
                const auto& term = value.terms.front();
                if (term.first.empty())
                    raw.push_back(ValueNormalizer::number(term.second));
                else if (term.first == "%")
                    raw.push_back(ValueNormalizer::percentage(term.second));
                else
                    raw.push_back(ValueNormalizer::dimension(term.second, term.first));
            }
            for (const ComponentValue& v : raw){
                if (v.isToken(TokenType::Dimension))
                    dimension(v, out);
                else
                    out.push_back(v);
            }
        }

        void math(const ComponentValue& v, ValueList& out){
            const std::string name = lowerAscii(v.name());
            const bool calcFamily = StaticCalc::isCalcFamily(name) || name == "-webkit-calc" || name == "-moz-calc";
            if (calcFamily && context.calcNative && context.compat().fork.has(ForkFeature::Calc)){
                // the fork evaluates the expression at layout time: it is kept when it is valid
                // (its types checked with any context; a var() without one static value is the
                // fork's to substitute) and holds no other math function, which the fork lacks
                CalcContext check;
                check.resolveVar = context.resolveVar;
                check.pxPerUnit = [](std::string_view){ return std::optional<double>(1.0); };
                std::string error;
                const bool valid = StaticCalc::evaluate(v, check, &error).has_value() || check.varUnresolved;
                if (!valid){
                    fail(codes::UnsupportedValue, "'" + serialize(v) + "' cannot be evaluated (" + error + "); declaration dropped",
                         "write the value without calc()");
                    return;
                }
                bool otherMath = false;
                for (const std::string& f : check.functions){
                    if (!StaticCalc::isCalcFamily(f))
                        otherMath = true;
                }
                if (!otherMath){
                    ComponentValue copy = shell(v);
                    if (name != "calc" && name != "min" && name != "max" && name != "clamp")
                        copy.token.value = "calc";
                    copy.children = list(v.children);
                    out.push_back(std::move(copy));
                    return;
                }
                // round(), sin(), ... inside: folded statically, as in every fork state
            }
            const bool calcPresent = calcFamily && context.compat().fork.has(ForkFeature::Calc);
            CalcContext calc;
            calc.resolveVar = context.resolveVar;
            calc.pxPerUnit = [this](std::string_view unit){ return pxPerUnit(unit); };
            std::string error;
            percentEstimated = false;
            std::optional<CalcValue> value = StaticCalc::evaluate(v, calc, &error);
            const std::string written = serialize(v);
            if (!value){
                if (calc.varUnresolved){
                    fail(codes::VarUntyped, "'" + written + "' uses a variable without one static value; declaration dropped",
                         "give the variable one value, or compute the value without var()");
                }else{
                    fail(codes::UnsupportedValue, "'" + written + "' cannot be evaluated (" + error + "); declaration dropped",
                         "write the value without calc()");
                }
                return;
            }
            bool otherMath = false;
            for (const std::string& f : calc.functions){
                if (!StaticCalc::isCalcFamily(f))
                    otherMath = true;
            }
            if (keepLengthSums && calcFamily && !otherMath && !calc.usedContext && value->terms.size() > 1){
                // a translation by a sum is the sum of translations: exact without calc()
                bool lengths = true;
                for (const auto& term : value->terms)
                    lengths = lengths && (term.first == "%" || (!term.first.empty() && isLengthCategory(unitCategory(term.first))));
                if (lengths){
                    ValueList children;
                    for (const auto& term : value->terms){
                        if (!children.empty()){
                            children.push_back(ValueNormalizer::whitespace());
                            children.push_back(ValueNormalizer::delim('+'));
                            children.push_back(ValueNormalizer::whitespace());
                        }
                        if (term.first == "%")
                            children.push_back(ValueNormalizer::percentage(term.second));
                        else
                            dimension(ValueNormalizer::dimension(term.second, term.first), children);
                    }
                    if (result.ok)
                        out.push_back(ValueNormalizer::function("calc", std::move(children)));
                    return;
                }
            }
            if (calc.usedContext && percentEstimated){
                result.dependsOnViewport = true;
                // (with the fork's Calc, only values a rewrite reads token by token get here: not a
                // fallback that a fork feature removes, an approximation of that rewrite)
                report(calcPresent ? codes::ApproximatedVisible : codes::ForkFallback, Severity::Warning,
                       "'" + written + "' compares a percentage with a length; it is decided for the current viewport" +
                           std::string(calcPresent ? " (the engine evaluates calc() where it reads a value as written, not in this rewritten one)" : ""),
                       "set the size with max-width / min-width, or flex");
            }else if (calc.usedContext){
                if (calc.usedViewport)
                    result.dependsOnViewport = true;
                report(codes::ViewportDependent, Severity::Info, "'" + written + "' compares values in different units; it is decided for the load-time context" +
                       std::string(calc.usedViewport ? " and re-evaluated when the viewport changes" : ""));
            }
            CalcValue folded = *value;
            if (folded.terms.size() > 1){
                // mixed units
                if (folded.has("%")){
                    if (context.percentBasis == PercentBasis::FontSize || context.percentBasis == PercentBasis::LineHeight){
                        CalcValue converted;
                        for (const auto& term : folded.terms)
                            converted.add(term.first == "%" ? "em" : term.first, term.first == "%" ? term.second / 100.0 : term.second);
                        folded = converted;
                    }else if (context.percentBasis == PercentBasis::Number){
                        CalcValue converted;
                        for (const auto& term : folded.terms)
                            converted.add(term.first == "%" ? "" : term.first, term.first == "%" ? term.second / 100.0 : term.second);
                        folded = converted;
                    }else{
                        const double percent = folded.get("%");
                        report(calcPresent ? codes::ApproximatedVisible : codes::ForkFallback, Severity::Warning,
                               "'" + written + "' is reduced to '" + formatNumber(percent) + "%'" +
                                   (calcPresent ? std::string(" (the engine evaluates calc() where it reads a value as written, not in this rewritten one)")
                                                : std::string(" until the fork has calc()")),
                               "size the element with box-sizing, padding or flex instead");
                        folded = CalcValue::of(percent, "%");
                    }
                }
                if (folded.terms.size() > 1){
                    std::optional<CalcValue> px = StaticCalc::resolveToPx(folded, calc);
                    if (!px || px->terms.size() > 1){
                        fail(codes::UnsupportedValue, "'" + written + "' mixes units that cannot be combined statically; declaration dropped",
                             "write the value in one unit");
                        return;
                    }
                    if (calc.usedViewport)
                        result.dependsOnViewport = true;
                    report(codes::ViewportDependent, Severity::Info, "'" + written + "' is evaluated for the load-time context as " +
                           formatNumber(px->terms.empty() ? 0.0 : px->terms.front().second) + "px");
                    folded = *px;
                }
            }else if (otherMath){
                report(codes::ApproximatedMinor, Severity::Info, "'" + written + "' is computed at load (the engine has no math functions)");
            }
            emitCalc(folded, out);
        }

        void env(const ComponentValue& v, ValueList& out){
            std::vector<ValueList> args = splitAtCommas(v.children);
            std::string name;
            if (!args.empty()){
                ValueList first = trimWhitespace(args[0]);
                if (!first.empty() && first[0].isToken(TokenType::Ident))
                    name = lowerAscii(first[0].token.value);
            }
            ValueList fallback;
            if (args.size() > 1){
                for (size_t i = 1; i < args.size(); i++){
                    if (i > 1)
                        fallback.push_back(ValueNormalizer::comma());
                    for (const ComponentValue& c : args[i])
                        fallback.push_back(c);
                }
                fallback = trimWhitespace(fallback);
            }
            if (name.compare(0, 16, "safe-area-inset-") == 0){
                report(codes::ApproximatedMinor, Severity::Info, "'" + serialize(v) + "' is 0px (no safe area)");
                out.push_back(ValueNormalizer::dimension(0, "px"));
                return;
            }
            if (!fallback.empty()){
                report(codes::ApproximatedMinor, Severity::Info, "'" + serialize(v) + "' uses its fallback (the variable is not defined here)");
                ValueList normalized = list(fallback);
                out.insert(out.end(), normalized.begin(), normalized.end());
                return;
            }
            fail(codes::UnsupportedValue, "'" + serialize(v) + "' has no value here and no fallback; declaration dropped",
                 "give env() a fallback");
        }

        void var(const ComponentValue& v, ValueList& out){
            ComponentValue copy = shell(v);
            bool afterComma = false;
            ValueList fallback;
            for (const ComponentValue& child : v.children){
                if (afterComma){
                    fallback.push_back(child);
                    continue;
                }
                copy.children.push_back(child);
                if (child.isToken(TokenType::Comma))
                    afterComma = true;
            }
            if (afterComma){
                ValueList normalized = list(fallback);
                copy.children.insert(copy.children.end(), normalized.begin(), normalized.end());
            }
            out.push_back(std::move(copy));
        }

        void one(const ComponentValue& v, ValueList& out){
            if (v.kind == ComponentValue::Kind::Token){
                const Token& t = v.token;
                if (context.colors && (t.type == TokenType::Hash || (t.type == TokenType::Ident && ColorValue::isColorSyntax(v)))){
                    color(v, out);
                    return;
                }
                if (t.type == TokenType::Dimension){
                    dimension(v, out);
                    return;
                }
                out.push_back(v);
                return;
            }
            if (v.kind == ComponentValue::Kind::Block){
                out.push_back(shell(v));
                out.back().children = list(v.children);
                return;
            }
            const std::string name = lowerAscii(v.name());
            if (name == "url" || name == "format" || name == "tech" || name == "local"){
                out.push_back(v);
                return;
            }
            if (name == "var"){
                var(v, out);
                return;
            }
            if (name == "env"){
                env(v, out);
                return;
            }
            if (name == "attr"){
                fail(codes::UnsupportedFunction, "'" + serialize(v) + "' is not supported; declaration dropped",
                     "write the value in the CSS, or set it from Lua");
                return;
            }
            if (StaticCalc::isMathFunction(name)){
                math(v, out);
                return;
            }
            if ((name == "translate" || name == "translatex" || name == "translatey" || name == "translatez" || name == "translate3d") &&
                !(context.calcNative && context.compat().fork.has(ForkFeature::Calc))){
                translate(v, out);
                return;
            }
            if (context.colors && ColorValue::isColorFunction(name)){
                color(v, out);
                return;
            }
            ComponentValue copy = shell(v);
            copy.children = list(v.children);
            out.push_back(std::move(copy));
        }

        // a copy of v without its children (the caller sets them)
        static ComponentValue shell(const ComponentValue& v){
            ComponentValue copy;
            copy.kind = v.kind;
            copy.token = v.token;
            copy.offset = v.offset;
            copy.length = v.length;
            copy.closed = v.closed;
            return copy;
        }

        const NormalizeContext& context;
        struct Pending {
            DiagCode code;
            Severity severity;
            std::string message, suggestion;
        };
        std::vector<Pending> pendingReports;
        // in a translation folded without the fork's calc(): a sum of lengths stays calc(a + b),
        // split into one translation per term (splitTranslate, or the 'translate' rewrite)
        bool keepLengthSums = false;
        std::set<std::string> reportedApproximations;
        bool percentEstimated = false;
    };

}

const ComponentValue* jasperengine::htmlui::compat::soleValue(const ValueList& values){
    const ComponentValue* sole = nullptr;
    for (const ComponentValue& v : values){
        if (v.isWhitespace())
            continue;
        if (sole)
            return nullptr;
        sole = &v;
    }
    return sole;
}

void jasperengine::htmlui::compat::trimInPlace(ValueList& values){
    while (!values.empty() && values.back().isWhitespace())
        values.pop_back();
    size_t leading = 0;
    while (leading < values.size() && values[leading].isWhitespace())
        leading++;
    if (leading)
        values.erase(values.begin(), values.begin() + (std::ptrdiff_t)leading);
}

std::vector<ComponentValue> ValueNormalizer::lengthSumTerms(const ValueList& value){
    std::vector<ComponentValue> terms;
    const ComponentValue* sole = soleValue(value);
    if (!sole || sole->kind != ComponentValue::Kind::Function || !equalsNoCase(sole->name(), "calc"))
        return terms;
    for (const ComponentValue& c : sole->children){
        if (c.isWhitespace() || (c.isToken(TokenType::Delim) && c.token.value.view() == "+"))
            continue;
        if (!c.isToken(TokenType::Dimension) && !c.isToken(TokenType::Percentage) && !c.isToken(TokenType::Number))
            return std::vector<ComponentValue>();
        terms.push_back(c);
    }
    if (terms.size() < 2)
        terms.clear();
    return terms;
}

NormalizeResult ValueNormalizer::normalize(const ValueList& value, const NormalizeContext& context){
    return normalize(value, context, ValueList());
}

NormalizeResult ValueNormalizer::normalize(const ValueList& value, const NormalizeContext& context,
                                           ValueList&& storage){
    Walker walker(context);
    ValueList out = walker.list(value, std::move(storage));
    walker.flushReports();
    NormalizeResult result = walker.result;
    result.value = std::move(out);
    return result;
}
