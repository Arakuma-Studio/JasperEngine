// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effects (phase-2 §3.9.7 "Effects"): filter functions (RmlUi takes drop-shadow with the color
// first), clip-path basic shapes through the html-shape mask decorator.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    // a filter list for RmlUi; dropped: url() filters removed. nullopt when malformed
    std::optional<std::string> filters(Session& session, const std::vector<ComponentValue>& value, const std::string& currentColor,
                                       bool& droppedUrl){
        std::vector<std::string> out;
        droppedUrl = false;
        for (const ComponentValue& v : words(value)){
            if (v.isToken(TokenType::Url) || v.isFunction("url")){
                droppedUrl = true;
                continue;
            }
            if (v.kind != ComponentValue::Kind::Function)
                return std::nullopt;
            const std::string name = lowerAscii(v.name());
            std::vector<ComponentValue> args = words(v.children);
            if (name == "drop-shadow"){
                std::vector<std::string> lengths;
                std::string color;
                for (const ComponentValue& a : args){
                    if (isLength(session, a))
                        lengths.push_back(lengthText(a));
                    else if (isColor(session, a) || isVar(a))
                        color = text(a);
                    else
                        return std::nullopt;
                }
                if (lengths.size() < 2 || lengths.size() > 3)
                    return std::nullopt;
                if (lengths.size() == 2)
                    lengths.push_back("0px");
                out.push_back("drop-shadow(" + (color.empty() ? currentColor : color) + " " + join(lengths) + ")");
                continue;
            }
            if (name == "blur"){
                if (args.empty()){
                    out.push_back("blur(0px)");
                    continue;
                }
                if (args.size() != 1 || !isLength(session, args[0]))
                    return std::nullopt;
                out.push_back("blur(" + lengthText(args[0]) + ")");
                continue;
            }
            if (name == "hue-rotate"){
                if (args.empty()){
                    out.push_back("hue-rotate(0deg)");
                    continue;
                }
                if (args.size() != 1)
                    return std::nullopt;
                if (args[0].isToken(TokenType::Number) && args[0].token.number == 0){
                    out.push_back("hue-rotate(0deg)");
                    continue;
                }
                if (!(args[0].isToken(TokenType::Dimension) && unitCategory(args[0].token.unit) == UnitCategory::Angle) && !isVar(args[0]))
                    return std::nullopt;
                out.push_back("hue-rotate(" + text(args[0]) + ")");
                continue;
            }
            if (name == "brightness" || name == "contrast" || name == "grayscale" || name == "invert" || name == "opacity" ||
                name == "saturate" || name == "sepia"){
                if (args.empty()){
                    out.push_back(name + "(1)");
                    continue;
                }
                if (args.size() != 1 || !(args[0].isToken(TokenType::Number) || args[0].isToken(TokenType::Percentage) || isVar(args[0])))
                    return std::nullopt;
                out.push_back(name + "(" + text(args[0]) + ")");
                continue;
            }
            return std::nullopt;
        }
        return join(out);
    }

    std::string classifyFilters(Session& session, const std::vector<ComponentValue>& value){
        std::vector<ComponentValue> w = words(value);
        if (w.size() == 1 && isIdent(w[0], "none"))
            return "filters";
        bool droppedUrl = false;
        return filters(session, value, "#000000ff", droppedUrl) ? "filters" : std::string();
    }

    std::optional<std::string> transformFilters(Session& session, const std::vector<ComponentValue>& value, const std::string&, SourceRef){
        std::vector<ComponentValue> w = words(value);
        if (w.size() == 1 && isIdent(w[0], "none"))
            return std::string("none");
        bool droppedUrl = false;
        return filters(session, value, serialize(ValueNormalizer::currentColorCompanion()), droppedUrl);
    }

}

const VarFamily jasperengine::htmlui::compat::kFilterFamily = {"filter", classifyFilters, transformFilters};

void jasperengine::htmlui::compat::rewriteFilter(DeclContext& context){
    std::string name = context.name;
    if (name == "-webkit-backdrop-filter")
        name = "backdrop-filter";
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "none")){
        context.emit(name, "none");
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        std::optional<TypedVar> typed = wholeVar(context, "filter");
        context.emit(name, typed ? typed->reference : text(w[0]));
        return;
    }
    const std::string ruleColor = context.rule ? ruleStaticColor(context.session, *context.rule) : std::string();
    const std::string currentColor = ruleColor.empty() ? serialize(ValueNormalizer::currentColorCompanion()) : ruleColor;
    bool droppedUrl = false;
    std::optional<std::string> list = filters(context.session, context.value, currentColor, droppedUrl);
    if (!list){
        unsupportedValue(context, "use blur(), brightness(), contrast(), drop-shadow(), grayscale(), hue-rotate(), invert(), opacity(), saturate(), sepia()");
        return;
    }
    if (droppedUrl)
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "url() filters (SVG filters) are not supported; dropped from the list",
                         "use the CSS filter functions");
    if (list->empty()){
        context.dropped = true;
        return;
    }
    context.emit(name, *list);
}

std::optional<std::string> jasperengine::htmlui::compat::clipPathShape(DeclContext& context, const Declaration& declaration){
    NormalizeContext normalize;
    normalize.property = "clip-path";
    normalize.options = context.session.options;
    normalize.resolveVar = [&context](const ComponentValue& var, std::vector<ComponentValue>& out){ return context.session.staticVar(var, out); };
    NormalizeResult normalized = ValueNormalizer::normalize(declaration.value, normalize);
    if (!normalized.ok)
        return std::nullopt;
    std::vector<ComponentValue> w = words(normalized.value);
    if (w.size() == 1 && isIdent(w[0], "none"))
        return std::nullopt;
    std::string shape, box;
    for (const ComponentValue& v : w){
        const std::string k = identOf(v);
        if (k == "border-box" || k == "padding-box" || k == "content-box" || k == "margin-box" || k == "fill-box" || k == "stroke-box" ||
            k == "view-box"){
            box = k;
            continue;
        }
        if (v.isToken(TokenType::Url) || v.isFunction("url")){
            context.reportAt(declaration.ref, Severity::Warning, codes::UnsupportedValue, "clip-path: url() (an SVG clip path) is not supported; dropped",
                             "use a basic shape: inset(), circle(), ellipse(), polygon() or path()");
            return std::nullopt;
        }
        if (v.kind == ComponentValue::Kind::Function){
            const std::string name = lowerAscii(v.name());
            if (name == "inset" || name == "circle" || name == "ellipse" || name == "polygon" || name == "path" || name == "xywh" ||
                name == "rect"){
                // the shape travels inside a double-quoted string: its own strings use single quotes
                shape = serialize(v);
                for (char& c : shape){
                    if (c == '"')
                        c = '\'';
                }
                continue;
            }
        }
        context.reportAt(declaration.ref, Severity::Warning, codes::UnsupportedValue, "'clip-path: " + text(w) + "' is not supported; dropped",
                         "use a basic shape: inset(), circle(), ellipse(), polygon() or path()");
        return std::nullopt;
    }
    if (shape.empty()){
        if (box.empty())
            return std::nullopt;
        shape = "inset(0)";
    }
    context.reportAt(declaration.ref, Severity::Info, codes::ApproximatedMinor, "clip-path clips the drawing, not the pointer hit area");
    return "html-shape(" + serializeString(box.empty() ? shape : shape + " " + box) + ")";
}
