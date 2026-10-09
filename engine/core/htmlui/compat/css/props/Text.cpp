// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Text (phase-2 §3.9.7 "Text"): text-shadow and -webkit-text-stroke as RmlUi font effects,
// -webkit-text-fill-color, text-decoration lines, text-align keywords, the white-space family,
// word breaking, line clamping.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    // half of a length, unit kept ("8px" -> "4px")
    std::string half(const ComponentValue& v){
        if (v.isToken(TokenType::Number))
            return "0px";
        ComponentValue copy = v;
        copy.token.number = v.token.number / 2.0;
        return text(copy);
    }

    struct Shadow {
        ValueList lengths;
        std::string color;
    };

    // one shadow "x y [blur] [color]" (color first or last); false when malformed
    bool parseShadow(Session& session, const ValueList& group, Shadow& shadow){
        for (const ComponentValue& v : words(group)){
            if (isLength(session, v)){
                shadow.lengths.push_back(v);
            }else if (isColor(session, v) || isVar(v)){
                if (!shadow.color.empty())
                    return false;
                shadow.color = text(v);
            }else{
                return false;
            }
        }
        return shadow.lengths.size() >= 2 && shadow.lengths.size() <= 3;
    }

    // text-shadow -> font-effect list; nullopt when malformed
    std::optional<std::string> fontEffects(Session& session, const ValueList& value, const std::string& fallbackColor,
                                           bool* usedWhite){
        std::vector<std::string> effects;
        for (const ValueList& group : commaGroups(value)){
            Shadow shadow;
            if (!parseShadow(session, group, shadow))
                return std::nullopt;
            std::string color = shadow.color;
            if (color.empty()){
                color = fallbackColor.empty() ? "#ffffffff" : fallbackColor;
                if (fallbackColor.empty() && usedWhite)
                    *usedWhite = true;
            }
            const std::string x = lengthText(shadow.lengths[0]);
            const std::string y = lengthText(shadow.lengths[1]);
            const bool blurred = shadow.lengths.size() == 3 && !(shadow.lengths[2].isToken(TokenType::Number) && shadow.lengths[2].token.number == 0) &&
                                 !(shadow.lengths[2].isToken(TokenType::Dimension) && shadow.lengths[2].token.number == 0);
            if (blurred){
                if (shadow.lengths[2].token.number < 0)
                    return std::nullopt;
                effects.push_back("glow(0px " + half(shadow.lengths[2]) + " " + x + " " + y + " " + color + ")");
            }else{
                effects.push_back("shadow(" + x + " " + y + " " + color + ")");
            }
        }
        if (effects.empty())
            return std::nullopt;
        return join(effects, ", ");
    }

    std::string classifyShadows(Session& session, const ValueList& value){
        ValueList w = words(value);
        if (w.size() == 1 && isIdent(w[0], "none"))
            return "effects";
        return fontEffects(session, value, std::string(), nullptr) ? "effects" : std::string();
    }

    std::optional<std::string> transformShadows(Session& session, const ValueList& value, const std::string&, SourceRef){
        ValueList w = words(value);
        if (w.size() == 1 && isIdent(w[0], "none"))
            return std::string(companions::kNoEffect);
        return fontEffects(session, value, std::string(), nullptr);
    }

    std::string fallbackColorOf(DeclContext& context){
        if (context.rule)
            return ruleStaticColor(context.session, *context.rule);
        return std::string();
    }

    std::optional<std::string> borderLikeWidth(Session& session, const ComponentValue& v){
        const std::string k = identOf(v);
        if (k == "thin")
            return std::string("1px");
        if (k == "medium")
            return std::string("3px");
        if (k == "thick")
            return std::string("5px");
        if (isLength(session, v))
            return lengthText(v);
        return std::nullopt;
    }

    std::string ruleStroke(DeclContext& context, const char* property){
        if (!context.rule)
            return std::string();
        std::optional<ValueList> value = ruleValue(context.session, *context.rule, property);
        return value ? text(*value) : std::string();
    }

}

const VarFamily jasperengine::htmlui::compat::kFontEffectFamily = {"font-effect", classifyShadows, transformShadows};

void jasperengine::htmlui::compat::rewriteTextShadow(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "none")){
        companions::emitTextShadow(context, std::string());
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        std::optional<TypedVar> typed = wholeVar(context, "font-effect");
        companions::emitTextShadow(context, typed ? typed->reference : context.valueText());
        return;
    }
    bool usedWhite = false;
    std::optional<std::string> effects = fontEffects(context.session, context.value, fallbackColorOf(context), &usedWhite);
    if (!effects){
        unsupportedValue(context, "write text-shadow: <x> <y> [<blur>] [<color>], ...");
        return;
    }
    if (usedWhite)
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "text-shadow without a color uses white (font effects cannot follow currentColor)",
                         "give the shadow a color");
    companions::emitTextShadow(context, *effects);
}

void jasperengine::htmlui::compat::rewriteTextStroke(DeclContext& context){
    ValueList w = words(context.value);
    std::string width, color;
    if (context.name == "-webkit-text-stroke"){
        for (const ComponentValue& v : w){
            if (std::optional<std::string> l = borderLikeWidth(context.session, v)){
                width = *l;
            }else if (isColor(context.session, v) || isVar(v)){
                color = text(v);
            }else{
                unsupportedValue(context, "write -webkit-text-stroke: <width> <color>");
                return;
            }
        }
    }else if (context.name == "-webkit-text-stroke-width"){
        if (w.size() != 1 || !borderLikeWidth(context.session, w[0])){
            unsupportedValue(context);
            return;
        }
        width = *borderLikeWidth(context.session, w[0]);
        color = ruleStroke(context, "-webkit-text-stroke-color");
    }else{
        if (w.size() != 1 || !(isColor(context.session, w[0]) || isVar(w[0]))){
            unsupportedValue(context);
            return;
        }
        color = text(w[0]);
        width = ruleStroke(context, "-webkit-text-stroke-width");
        if (width.empty()){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "-webkit-text-stroke-color without a width in the rule is drawn 1px wide");
            width = "1px";
        }
    }
    if (width.empty() || width == "0px"){
        companions::emitTextStroke(context, std::string());
        return;
    }
    if (color.empty()){
        color = fallbackColorOf(context);
        if (color.empty()){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "a text stroke without a color uses white (font effects cannot follow currentColor)",
                             "give the stroke a color");
            color = "#ffffffff";
        }
    }
    companions::emitTextStroke(context, "outline(" + width + " " + color + ")");
}

void jasperengine::htmlui::compat::rewriteTextFillColor(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() != 1){
        unsupportedValue(context);
        return;
    }
    // gradient text: the background rewrite writes the color
    if (context.rule && w[0].isToken(TokenType::Hash) && w[0].token.value.size() == 8 && w[0].token.value.compare(6, 2, "00") == 0){
        for (const Declaration& d : context.rule->declarations){
            if (d.name == "background-clip" || d.name == "-webkit-background-clip"){
                ValueList v = words(d.value);
                if (!v.empty() && isIdent(v.back(), "text"))
                    return;
            }
        }
    }
    if (!(isColor(context.session, w[0]) || isVar(w[0]))){
        unsupportedValue(context);
        return;
    }
    context.emit("color", text(w[0]));
}

void jasperengine::htmlui::compat::rewriteTextDecoration(DeclContext& context){
    ValueList w = words(context.value);
    std::vector<std::string> lines;
    std::vector<std::string> dropped;
    for (const ComponentValue& v : w){
        const std::string k = identOf(v);
        if (k == "underline" || k == "overline" || k == "line-through" || k == "none"){
            lines.push_back(k);
        }else if (k == "blink"){
            dropped.push_back(k);
        }else if (isVar(v) && w.size() == 1){
            context.emit("text-decoration", text(v));
            return;
        }else if (context.name == "text-decoration-line"){
            unsupportedValue(context, "use underline, overline, line-through or none");
            return;
        }else{
            dropped.push_back(text(v));
        }
    }
    if (lines.empty()){
        if (!dropped.empty())
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "text-decoration '" + join(dropped) +
                             "' is dropped (only the line keyword is drawn)");
        context.dropped = true;
        return;
    }
    if (lines.size() > 1)
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "only the first decoration line '" + lines[0] + "' is drawn");
    if (!dropped.empty())
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "text-decoration '" + join(dropped) +
                         "' is dropped (color, style and thickness follow the text)");
    context.emit("text-decoration", lines[0]);
}

void jasperengine::htmlui::compat::rewriteTextAlign(DeclContext& context){
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    const bool rtl = context.session.rootDirection == "rtl";
    if (k == "left" || k == "right" || k == "center" || k == "justify"){
        context.emit("text-align", k);
        return;
    }
    if (k == "start"){
        context.emit("text-align", rtl ? "right" : "left");
        return;
    }
    if (k == "end"){
        context.emit("text-align", rtl ? "left" : "right");
        return;
    }
    if (k == "-webkit-center" || k == "-moz-center" || k == "-webkit-match-parent"){
        context.emit("text-align", k == "-webkit-match-parent" ? "left" : "center");
        return;
    }
    if (k == "match-parent"){
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'text-align: match-parent' is written as left");
        context.emit("text-align", "left");
        return;
    }
    if (k == "justify-all"){
        context.emit("text-align", "justify");
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("text-align", text(w[0]));
        return;
    }
    unsupportedValue(context, "use left, right, center or justify");
}

void jasperengine::htmlui::compat::rewriteWhiteSpace(DeclContext& context){
    const std::string& n = context.name;
    ValueList w = words(context.value);
    std::vector<std::string> k;
    for (const ComponentValue& v : w)
        k.push_back(identOf(v));
    if (w.size() == 1 && isVar(w[0])){
        if (n == "white-space")
            context.emit("white-space", text(w[0]));
        else
            unsupportedValue(context);
        return;
    }
    auto emit = [&](const std::string& value){
        if (!RcssGrammar::accepts("white-space", value)){
            unsupportedValue(context);
            return;
        }
        context.emit("white-space", value);
    };
    if (n == "white-space"){
        if (k.size() == 1){
            if (k[0] == "break-spaces"){
                emit("pre-wrap");
                return;
            }
            if (k[0] == "collapse"){
                emit("normal");
                return;
            }
            if (k[0] == "preserve"){
                emit("pre-wrap");
                return;
            }
            if (k[0] == "preserve-breaks"){
                emit("pre-line");
                return;
            }
            emit(k[0]);
            return;
        }
        // Text 4 longhand pairs: <collapse> <wrap-mode>
        std::string collapse = "collapse", mode = "wrap";
        for (const std::string& word : k){
            if (word == "wrap" || word == "nowrap")
                mode = word;
            else
                collapse = word;
        }
        if (collapse == "collapse")
            emit(mode == "nowrap" ? "nowrap" : "normal");
        else if (collapse == "preserve")
            emit(mode == "nowrap" ? "pre" : "pre-wrap");
        else if (collapse == "preserve-breaks")
            emit("pre-line");
        else if (collapse == "break-spaces")
            emit("pre-wrap");
        else
            unsupportedValue(context);
        return;
    }
    if (n == "white-space-collapse"){
        if (k.size() == 1){
            if (const ValueSupport* row = findValue(n, k[0])){
                std::string output = row->output;
                const std::string prefix = "white-space: ";
                if (output.compare(0, prefix.size(), prefix) == 0)
                    output = output.substr(prefix.size());
                emit(output);
                return;
            }
        }
        unsupportedValue(context, "use collapse, preserve or preserve-breaks");
        return;
    }
    // text-wrap, text-wrap-mode, text-wrap-style
    for (const std::string& word : k){
        if (word == "nowrap"){
            emit("nowrap");
            return;
        }
    }
    for (const std::string& word : k){
        if (word == "wrap"){
            emit("normal");
            return;
        }
    }
    if (!k.empty() && (k[0] == "balance" || k[0] == "pretty" || k[0] == "stable" || k[0] == "auto")){
        context.reportAs(Severity::Info, codes::IgnoredProperty, "'" + n + ": " + k[0] + "' has no effect in the engine; dropped");
        context.dropped = true;
        return;
    }
    unsupportedValue(context);
}

void jasperengine::htmlui::compat::rewriteOverflowWrap(DeclContext& context){
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    if (context.name == "word-break"){
        if (k == "normal" || k == "break-all" || k == "break-word"){
            context.emit("word-break", k);
            return;
        }
        if (k == "keep-all" || k == "auto-phrase"){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'word-break: " + k + "' is written as normal");
            context.emit("word-break", "normal");
            return;
        }
        if (w.size() == 1 && isVar(w[0])){
            context.emit("word-break", text(w[0]));
            return;
        }
        unsupportedValue(context, "use normal, break-all or break-word");
        return;
    }
    // overflow-wrap / word-wrap
    if (k == "normal")
        return;
    if (k == "break-word" || k == "anywhere"){
        context.emit("word-break", "break-word");
        return;
    }
    unsupportedValue(context, "use break-word");
}

void jasperengine::htmlui::compat::rewriteLineClamp(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "none"))
        return;
    if (w.size() != 1 || !w[0].isToken(TokenType::Number) || w[0].token.number < 1){
        unsupportedValue(context, "use a positive number of lines");
        return;
    }
    const double lines = std::floor(w[0].token.number);
    // the line height of the rule or its base rule
    std::optional<ValueList> lineHeight;
    if (context.rule){
        lineHeight = ruleValue(context.session, *context.rule, "line-height");
        if (!lineHeight){
            if (const FlatRule* base = BaseRules::find(context.session, *context.rule, [&](const FlatRule& candidate){
                    return ruleValue(context.session, candidate, "line-height").has_value();
                }))
                lineHeight = ruleValue(context.session, *base, "line-height");
        }
    }
    std::string maxHeight;
    if (lineHeight && lineHeight->size() == 1){
        const ComponentValue& v = (*lineHeight)[0];
        if (v.isToken(TokenType::Number))
            maxHeight = formatNumber(lines * v.token.number, 4) + "em";
        else if (v.isToken(TokenType::Percentage))
            maxHeight = formatNumber(lines * v.token.number / 100.0, 4) + "em";
        else if (v.isToken(TokenType::Dimension) && isLength(context.session, v))
            maxHeight = formatNumber(lines * v.token.number, 4) + lowerAscii(v.token.unit);
        else if (isIdent(v, "normal"))
            maxHeight = formatNumber(lines * 1.2, 4) + "em";
    }
    if (maxHeight.empty())
        maxHeight = formatNumber(lines * 1.2, 4) + "em";
    context.reportAs(Severity::Warning, codes::ApproximatedVisible, "'" + context.sourceText() + "' cuts the text at " +
                     formatNumber(lines) + " lines (max-height " + maxHeight + ") without an ellipsis",
                     "set a line-height on the rule so the cut is exact");
    context.emit("max-height", maxHeight);
    context.emit("overflow", "hidden");
}
