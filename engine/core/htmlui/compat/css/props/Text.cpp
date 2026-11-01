// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Text (phase-2 §3.9.7 "Text"): text-shadow and -webkit-text-stroke as RmlUi font effects,
// -webkit-text-fill-color, text-decoration lines, text-align keywords, the white-space family,
// word breaking, line clamping. With the fork's TextShadow and TextProperties (phase 3, p3-text)
// the text properties pass through natively, checked against the fork's grammar.

#include "htmlui/HtmlTextEffects.h"
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

    // one shadow "x y [blur] [color]" (color first or last); false when malformed
    struct Shadow {
        const ComponentValue* lengths[3] = {nullptr, nullptr, nullptr};
        size_t lengthCount = 0;
        const ComponentValue* color = nullptr;
    };

    bool parseShadow(Session& session, const ValueRange& group, Shadow& shadow){
        for (const ComponentValue* v : wordsOf(group)){
            if (isLength(session, *v)){
                if (shadow.lengthCount < 3)
                    shadow.lengths[shadow.lengthCount] = v;
                shadow.lengthCount++;
            }else if (isColor(session, *v) || isVar(*v)){
                if (shadow.color)
                    return false;
                shadow.color = v;
            }else{
                return false;
            }
        }
        return shadow.lengthCount >= 2 && shadow.lengthCount <= 3;
    }

    // lengthText() appended
    void appendLength(std::string& out, const ComponentValue& value){
        if (value.isToken(TokenType::Number) && value.token.number == 0)
            out += "0px";
        else
            appendSerialized(out, value);
    }

    // text-shadow -> font-effect list appended to `out`; false when malformed. A shadow without
    // a color takes the rule's static color (`context`'s rule, read only then), else white.
    bool fontEffects(Session& session, const ValueList& value, DeclContext* context, bool* usedWhite, std::string& out){
        bool fallbackRead = false;
        std::string fallbackColor;
        bool any = false;
        for (const ValueRange& group : commaRanges(value)){
            Shadow shadow;
            if (!parseShadow(session, group, shadow))
                return false;
            const ComponentValue& blur = *shadow.lengths[2 < shadow.lengthCount ? 2 : 0];
            const bool blurred = shadow.lengthCount == 3 && !(blur.isToken(TokenType::Number) && blur.token.number == 0) &&
                                 !(blur.isToken(TokenType::Dimension) && blur.token.number == 0);
            if (blurred && blur.token.number < 0)
                return false;
            if (any)
                out += ", ";
            any = true;
            if (blurred){
                out += "glow(0px ";
                if (blur.isToken(TokenType::Number)){
                    out += "0px";
                }else{
                    // half of the blur, unit kept ("8px" -> "4px")
                    ComponentValue half = blur;
                    half.token.number = blur.token.number / 2.0;
                    appendSerialized(out, half);
                }
                out += ' ';
            }else{
                out += "shadow(";
            }
            appendLength(out, *shadow.lengths[0]);
            out += ' ';
            appendLength(out, *shadow.lengths[1]);
            out += ' ';
            if (shadow.color){
                appendSerialized(out, *shadow.color);
            }else{
                if (!fallbackRead){
                    fallbackRead = true;
                    if (context && context->rule)
                        fallbackColor = ruleStaticColor(context->session, *context->rule);
                }
                if (fallbackColor.empty()){
                    out += "#ffffffff";
                    if (usedWhite)
                        *usedWhite = true;
                }else{
                    out += fallbackColor;
                }
            }
            out += ')';
        }
        return any;
    }

    std::string fallbackColorOf(DeclContext& context){
        if (context.rule)
            return ruleStaticColor(context.session, *context.rule);
        return std::string();
    }

    // (one buffer for the font effects of the thread: the emitted value is a copy)
    std::string& effectBuffer(){
        static thread_local std::string buffer;
        return buffer;
    }

    std::string classifyShadows(Session& session, const ValueList& value){
        const Words w = wordsOf(value);
        if (w.size() == 1 && isIdent(*w[0], "none"))
            return "effects";
        std::string effects;
        return fontEffects(session, value, nullptr, nullptr, effects) ? "effects" : std::string();
    }

    std::optional<std::string> transformShadows(Session& session, const ValueList& value, const std::string&, SourceRef){
        const Words w = wordsOf(value);
        if (w.size() == 1 && isIdent(*w[0], "none"))
            return std::string(companions::kNoEffect);
        std::string effects;
        if (!fontEffects(session, value, nullptr, nullptr, effects))
            return std::nullopt;
        return effects;
    }

    // The declaration written as it is for the fork's own property (phase 3): its value checked
    // against the fork's grammar, else the declaration is dropped as an unsupported value.
    void emitNative(DeclContext& context, const std::string& property, const std::string& suggestion = std::string()){
        const std::string value = context.valueText();
        if (!RcssGrammar::accepts(property, value, context.session.options.fork)){
            unsupportedValue(context, suggestion);
            return;
        }
        context.emit(property, value);
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
    const Words w = wordsOf(context.value);
    if (context.session.fork(ForkFeature::TextShadow)){
        // native: blur, several shadows and currentcolor as in CSS (ElementText draws them)
        emitNative(context, "text-shadow", "write text-shadow: <x> <y> [<blur>] [<color>], ...");
        return;
    }
    if (w.size() == 1 && isIdent(*w[0], "none")){
        companions::emitTextShadow(context, std::string_view());
        return;
    }
    if (w.size() == 1 && isVar(*w[0])){
        std::optional<TypedVar> typed = wholeVar(context, "font-effect");
        companions::emitTextShadow(context, typed ? typed->reference : context.valueText());
        return;
    }
    bool usedWhite = false;
    std::string& effects = effectBuffer();
    effects.clear();
    if (!fontEffects(context.session, context.value, &context, &usedWhite, effects)){
        unsupportedValue(context, "write text-shadow: <x> <y> [<blur>] [<color>], ...");
        return;
    }
    if (usedWhite)
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "text-shadow without a color uses white (font effects cannot follow currentColor)",
                         "give the shadow a color");
    companions::emitTextShadow(context, effects);
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
    if (context.session.fork(ForkFeature::TextProperties)){
        // native: the shorthand and its longhands; a wavy line is drawn solid
        for (const ComponentValue& v : w){
            if (identOf(v) == "wavy" && (context.name == "text-decoration" || context.name == "text-decoration-style"))
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "a wavy text decoration is drawn solid", "use solid, double, dotted or dashed");
        }
        emitNative(context, context.name);
        return;
    }
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
    if (context.session.fork(ForkFeature::Justify)){
        // p6-justify: the fork justifies; 'justify-all' is not parsed by Chromium, the declaration is dropped as there
        if (k == "justify-all"){
            context.reportAs(Severity::Warning, codes::UnsupportedValue, "'text-align: justify-all' is not supported (Chromium does not parse it); declaration dropped",
                             "write text-align: justify with text-align-last: justify");
            context.dropped = true;
            return;
        }
    }else if (k == "justify" || k == "justify-all"){
        // without the fork's justification (phase-3 spec 2.2): written as is, the fork aligns it to the start
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'text-align: " + k + "' does not justify the lines",
                         "Justified text is aligned left.");
    }
    if (context.session.fork(ForkFeature::TextProperties)){
        // native: start, end and match-parent follow the element's direction
        if (k == "left" || k == "right" || k == "center" || k == "justify" || k == "start" || k == "end" || k == "match-parent" ||
            k == "-webkit-center" || k == "-webkit-left" || k == "-webkit-right"){
            context.emit("text-align", k);
            return;
        }
        if (k == "-moz-center"){
            context.emit("text-align", "center");
            return;
        }
        if (k == "-webkit-match-parent"){
            context.emit("text-align", "match-parent");
            return;
        }
        if (k == "justify-all"){
            context.emit("text-align", "justify");
            return;
        }
    }
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
    if (k == "-webkit-left" || k == "-webkit-right"){
        context.emit("text-align", k == "-webkit-left" ? "left" : "right");
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
    if (context.session.fork(ForkFeature::TextProperties) && n != "text-wrap-style"){
        // native: white-space is the shorthand of white-space-collapse and text-wrap-mode
        if (n == "text-wrap"){
            for (const std::string& word : k){
                if (word == "balance" || word == "pretty" || word == "stable")
                    context.reportAs(Severity::Info, codes::ApproximatedMinor, "'text-wrap: " + word + "' wraps lines greedily in the engine");
            }
        }
        emitNative(context, n);
        return;
    }
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
    if (context.session.fork(ForkFeature::TextProperties)){
        // native: overflow-wrap (and its alias word-wrap), word-break with keep-all
        if (context.name == "word-break" && k == "auto-phrase"){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'word-break: auto-phrase' is written as normal");
            context.emit("word-break", "normal");
            return;
        }
        emitNative(context, context.name == "word-wrap" ? std::string("overflow-wrap") : context.name);
        return;
    }
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
    if (context.fork(ForkFeature::LineClamp)){
        // The fork's '-webkit-line-clamp' (P6-L1), Chromium 153's: none, or an integer from 1 (a var() is the fork's).
        if (w.size() == 1 && isVar(w[0])){
            context.emit("-webkit-line-clamp", text(w[0]));
            return;
        }
        const bool none = (w.size() == 1 && isIdent(w[0], "none"));
        const bool count = (w.size() == 1 && w[0].isToken(TokenType::Number) && w[0].token.number >= 1 &&
                            w[0].token.number == std::floor(w[0].token.number) && w[0].token.number <= 16777216);
        if (!none && !count){
            unsupportedValue(context, "use none or a whole number of lines from 1");
            return;
        }
        context.emit("-webkit-line-clamp", none ? std::string("none") : formatNumber(w[0].token.number));
        if (none || !context.rule)
            return;
        // Chromium clamps a 'display: -webkit-box' with a vertical '-webkit-box-orient' only: say so when the rule
        // itself shows the clamp has no effect (another rule may still declare what is missing).
        const std::optional<ValueList> display = ruleValue(context.session, *context.rule, "display");
        const std::optional<ValueList> orient = ruleValue(context.session, *context.rule, "-webkit-box-orient");
        const std::string displayKeyword = (display && display->size() == 1 ? identOf((*display)[0]) : std::string());
        const std::string orientKeyword = (orient && orient->size() == 1 ? identOf((*orient)[0]) : std::string());
        const char* pattern = "display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: <n>; overflow: hidden";
        if (!displayKeyword.empty() && displayKeyword != "-webkit-box" && displayKeyword != "-webkit-inline-box"){
            context.reportAs(Severity::Info, codes::IgnoredProperty, "'" + context.sourceText() + "' has no effect on 'display: " + displayKeyword +
                             "' (as in Chromium: it clamps a display: -webkit-box only)", std::string("use ") + pattern);
        }else if (!displayKeyword.empty() && (orientKeyword.empty() || orientKeyword == "horizontal" || orientKeyword == "inline-axis")){
            context.reportAs(Severity::Info, codes::IgnoredProperty, "'" + context.sourceText() + "' has no effect without '-webkit-box-orient: vertical'"
                             " (as in Chromium: the box is a horizontal flex container)", std::string("use ") + pattern);
        }
        return;
    }
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
                     "for an ellipsis use one line with text-overflow: ellipsis; set a line-height on the rule so the cut is exact");
    context.emit("max-height", maxHeight);
    context.emit("overflow", "hidden");
}

void jasperengine::htmlui::compat::rewriteUserSelect(DeclContext& context){
    // (reached with the fork's TextProperties: the row is Ignored without it)
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    if (k == "text" || k == "all" || k == "contain")
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'user-select: " + k + "' selects nothing: text outside text inputs is not selectable",
                         "only text inputs and text areas select text");
    emitNative(context, "user-select");
}

// text-effect (engine extension, ui-scaling-text spec 7.4): validated by its parser, written as declared
void jasperengine::htmlui::compat::rewriteTextEffect(DeclContext& context){
    const std::string value = context.valueText();
    std::string error;
    bool valid = true;
    if (context.name == "text-effect"){
        HtmlTextEffect parsed;
        valid = parseTextEffect(value, parsed, error);
    }else if (context.name == "text-effect-iteration"){
        int iterations = 0;
        valid = parseTextEffectIteration(value, iterations);
        if (!valid) error = "use infinite or a whole number from 1";
    }else if (context.name == "text-effect-time"){
        const std::string time = value;
        valid = time == "ui" || time == "game";
        if (!valid) error = "use ui (unscaled) or game";
    }
    if (!valid){
        context.report(codes::UnsupportedValue, context.name + ": " + error + "; the declaration is dropped",
                       "e.g. text-effect: wave(0.15em 1.5s), gradient(2s add #ff0 0%, #f80 100%)");
        context.dropped = true;
        return;
    }
    context.emit(context.name, value);
}
