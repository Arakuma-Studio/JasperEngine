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
        explicit Walker(const NormalizeContext& context): context(context) {}

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
            if (context.report)
                context.report(code, severity, message, suggestion);
        }

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
            ColorParseInfo info;
            bool clipped = false;
            std::optional<CssColor> parsed = ColorValue::parse(v, info, colorContext());
            if (info.currentColor){
                if (context.keepCurrentColorKeyword){
                    out.push_back(ValueNormalizer::ident("currentcolor"));
                }else if (!context.staticColor.empty()){
                    out.push_back(ValueNormalizer::hash(context.staticColor.substr(1)));
                }else{
                    out.push_back(ValueNormalizer::currentColorCompanion());
                    result.currentColor = true;
                }
                return;
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
                    const double vw = context.options.viewport.x / 100.0;
                    const double vh = context.options.viewport.y / 100.0;
                    const double perUnit = unit.unit == "vmin" ? std::min(vw, vh) : std::max(vw, vh);
                    out.push_back(ValueNormalizer::dimension(t.number * perUnit, "px"));
                    result.dependsOnViewport = true;
                    report(codes::ViewportDependent, Severity::Info, "'" + serialize(v) + "' (in a style attribute or keyframe, which has no per-orientation copy) is evaluated for the current viewport (" +
                           std::to_string(context.options.viewport.x) + "x" + std::to_string(context.options.viewport.y) + ")");
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
            const double vw = context.options.viewport.x / 100.0;
            const double vh = context.options.viewport.y / 100.0;
            if (u == "em") return context.emPx;
            if (u == "rem") return context.remPx;
            if (u == "vw" || u == "vi" || u == "dvw" || u == "svw" || u == "lvw") return vw;
            if (u == "vh" || u == "vb" || u == "dvh" || u == "svh" || u == "lvh") return vh;
            if (u == "vmin") return std::min(vw, vh);
            if (u == "vmax") return std::max(vw, vh);
            if (u == "ch" || u == "ex") return context.emPx * 0.5;
            if (u == "cap") return context.emPx * 0.7;
            if (u == "ic") return context.emPx;
            if (u == "lh") return context.emPx * 1.2;
            if (u == "rlh") return context.remPx * 1.2;
            if (u == "dp") return context.options.dpRatio;
            if (u == "%"){
                // a percentage compared with lengths: its basis estimated with the viewport
                if (context.percentBasis == PercentBasis::FontSize)
                    return context.emPx / 100.0;
                if (context.percentBasis == PercentBasis::LineHeight)
                    return context.emPx / 100.0;
                if (context.percentBasis == PercentBasis::ContainingBlock){
                    percentEstimated = true;
                    return context.vertical ? vh : vw;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        // the folded value as component values (relative units mapped like any dimension)
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
            if (calcFamily && context.options.fork.has(ForkFeature::Calc)){
                ComponentValue copy = shell(v);
                if (name != "calc" && name != "min" && name != "max" && name != "clamp")
                    copy.token.value = "calc";
                copy.children = list(v.children);
                out.push_back(std::move(copy));
                return;
            }
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
            if (calc.usedContext && percentEstimated){
                result.dependsOnViewport = true;
                report(codes::ForkFallback, Severity::Warning, "'" + written + "' compares a percentage with a length; it is decided for the current viewport",
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
                        report(codes::ForkFallback, Severity::Warning, "'" + written + "' is reduced to '" + formatNumber(percent) +
                               "%' until the fork has calc()", "size the element with box-sizing, padding or flex instead");
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

NormalizeResult ValueNormalizer::normalize(const ValueList& value, const NormalizeContext& context){
    return normalize(value, context, ValueList());
}

NormalizeResult ValueNormalizer::normalize(const ValueList& value, const NormalizeContext& context,
                                           ValueList&& storage){
    Walker walker(context);
    ValueList out = walker.list(value, std::move(storage));
    NormalizeResult result = walker.result;
    result.value = std::move(out);
    return result;
}
