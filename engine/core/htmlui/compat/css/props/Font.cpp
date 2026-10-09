// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fonts (phase-2 §3.9.7 "Fonts"): font-family lists written unquoted (RmlUi's string parser
// rejects quotes; the engine's font engine splits the list into a fallback chain), the font
// shorthand as longhands, size keywords, root sizes in px, relative weights, oblique,
// line-height: normal.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/StaticCalc.h"
#include "htmlui/compat/css/Units.h"

#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    const std::set<std::string>& genericFamilies(){
        static const std::set<std::string> generics = {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui",
                                                       "ui-sans-serif", "ui-serif", "ui-monospace", "ui-rounded", "emoji", "math",
                                                       "fangsong"};
        return generics;
    }

    // the family names of a font-family list; false when it is not one
    bool familyNames(const std::vector<ComponentValue>& value, std::vector<std::string>& names){
        for (const std::vector<ComponentValue>& group : splitAtCommas(value)){
            std::string name;
            bool quoted = false;
            for (const ComponentValue& v : trimWhitespace(group)){
                if (v.isToken(TokenType::String)){
                    if (!name.empty())
                        return false;
                    name = v.token.value;
                    quoted = true;
                }else if (v.isToken(TokenType::Ident)){
                    if (quoted)
                        return false;
                    name += (name.empty() ? "" : " ") + v.token.value;
                }else if (v.isWhitespace() || v.isToken(TokenType::Comment)){
                    continue;
                }else{
                    return false;
                }
            }
            // trimmed, inner quotes and commas removed (RCSS reads the list as one string)
            std::string clean;
            for (char c : name){
                if (c == '"' || c == '\'' || c == ',' || c == ';')
                    continue;
                clean += c;
            }
            while (!clean.empty() && clean.back() == ' ')
                clean.pop_back();
            while (!clean.empty() && clean.front() == ' ')
                clean.erase(clean.begin());
            if (clean.empty())
                return false;
            if (!quoted && genericFamilies().count(lowerAscii(clean)))
                clean = lowerAscii(clean);
            names.push_back(clean);
        }
        return !names.empty();
    }

    std::string familyList(const std::vector<std::string>& names){
        return join(names, ", ");
    }

    std::string classifyFamilies(Session&, const std::vector<ComponentValue>& value){
        std::vector<std::string> names;
        return familyNames(value, names) ? "list" : std::string();
    }

    std::optional<std::string> transformFamilies(Session& session, const std::vector<ComponentValue>& value, const std::string&, SourceRef){
        std::vector<std::string> names;
        if (!familyNames(value, names))
            return std::nullopt;
        for (const std::string& n : names)
            session.result->fontFamilies.push_back(n);
        return familyList(names);
    }

    bool isRootRule(const DeclContext& context){
        if (!context.rule)
            return false;
        for (const ComplexSelector& s : context.rule->selectors){
            if (SelectorRewriter::isRootRule(s, context.session.selectorOptions))
                return true;
        }
        return false;
    }

    std::optional<std::string> sizeKeyword(const std::string& k){
        if (const ValueSupport* row = findValue("font-size", k)){
            if (row->output[0] >= '0' && row->output[0] <= '9')
                return std::string(row->output);
        }
        return std::nullopt;
    }

    bool isWeightKeyword(const std::string& k){
        return k == "bold" || k == "bolder" || k == "lighter";
    }

    bool isStretchKeyword(const std::string& k){
        return k == "ultra-condensed" || k == "extra-condensed" || k == "condensed" || k == "semi-condensed" || k == "semi-expanded" ||
               k == "expanded" || k == "extra-expanded" || k == "ultra-expanded";
    }

}

const VarFamily jasperengine::htmlui::compat::kFontFamilyFamily = {"font-family", classifyFamilies, transformFamilies};

void jasperengine::htmlui::compat::rewriteFontFamily(DeclContext& context){
    if (context.value.size() == 1 && isVar(context.value[0])){
        if (std::optional<TypedVar> typed = wholeVar(context, "font-family"))
            context.emit("font-family", typed->reference);
        else
            context.emit("font-family", context.valueText());
        return;
    }
    std::vector<std::string> names;
    if (!familyNames(context.value, names)){
        unsupportedValue(context, "write a comma-separated list of family names");
        return;
    }
    for (const std::string& n : names)
        context.session.result->fontFamilies.push_back(n);
    context.emit("font-family", familyList(names));
}

void jasperengine::htmlui::compat::rewriteFontSize(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() != 1){
        unsupportedValue(context);
        return;
    }
    const ComponentValue& v = w[0];
    if (isVar(v)){
        context.emit("font-size", text(v));
        return;
    }
    if (v.isToken(TokenType::Ident)){
        if (std::optional<std::string> px = sizeKeyword(identOf(v))){
            context.emit("font-size", *px);
            return;
        }
        if (const ValueSupport* row = findValue("font-size", identOf(v))){
            context.emit("font-size", row->output);
            return;
        }
        unsupportedValue(context, "use a length, a percentage or a size keyword");
        return;
    }
    if (isRootRule(context) && (v.isToken(TokenType::Percentage) || (v.isToken(TokenType::Dimension) && equalsNoCase(v.token.unit, "em")))){
        // the RML root has no parent font size: CSS resolves it against the initial 16px
        const double px = v.isToken(TokenType::Percentage) ? 16.0 * v.token.number / 100.0 : 16.0 * v.token.number;
        context.emit("font-size", formatNumber(px, 4) + "px");
        return;
    }
    if (v.isToken(TokenType::Number) && v.token.number == 0){
        context.emit("font-size", "0px");
        return;
    }
    if (!RcssGrammar::accepts("font-size", text(v))){
        unsupportedValue(context, "use a length or a percentage");
        return;
    }
    context.emit("font-size", text(v));
}

void jasperengine::htmlui::compat::rewriteFontWeight(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1){
        const std::string k = identOf(w[0]);
        if (k == "bolder" || k == "lighter"){
            const std::string value = k == "bolder" ? "700" : "100";
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'font-weight: " + k + "' is written as " + value +
                             " (relative to a normal parent weight)", "write the numeric weight");
            context.emit("font-weight", value);
            return;
        }
        if (w[0].isToken(TokenType::Number) && (w[0].token.number < 1 || w[0].token.number > 1000)){
            unsupportedValue(context, "use a weight between 1 and 1000");
            return;
        }
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("font-weight", text(w[0]));
        return;
    }
    if (!RcssGrammar::accepts("font-weight", context.valueText())){
        unsupportedValue(context, "use normal, bold or a number");
        return;
    }
    context.emit("font-weight", context.valueText());
}

void jasperengine::htmlui::compat::rewriteFontStyle(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    const std::string k = w.empty() ? std::string() : identOf(w[0]);
    if (k == "oblique"){
        context.emit("font-style", "italic");
        return;
    }
    if ((k == "normal" || k == "italic") && w.size() == 1){
        context.emit("font-style", k);
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("font-style", text(w[0]));
        return;
    }
    unsupportedValue(context, "use normal or italic");
}

void jasperengine::htmlui::compat::rewriteLineHeight(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "normal")){
        context.emit("line-height", "1.2");
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("line-height", text(w[0]));
        return;
    }
    if (!RcssGrammar::accepts("line-height", context.valueText())){
        unsupportedValue(context, "use a number, a length or a percentage");
        return;
    }
    context.emit("line-height", context.valueText());
}

void jasperengine::htmlui::compat::rewriteFont(DeclContext& context){
    std::vector<ComponentValue> value = context.value;
    for (const ComponentValue& v : value){
        if (isVar(v)){
            context.reportAs(Severity::Warning, codes::VarUntyped, "'font' with var() cannot be split into longhands; written as is",
                             "set font-size and font-family separately");
            context.emit("font", context.valueText());
            return;
        }
    }
    std::vector<ComponentValue> w = words(value);
    if (w.size() == 1 && w[0].isToken(TokenType::Ident)){
        const std::string k = identOf(w[0]);
        if (k == "caption" || k == "icon" || k == "menu" || k == "message-box" || k == "small-caption" || k == "status-bar"){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "system font '" + k + "' is written as 13.333px system-ui");
            context.emit("font-style", "normal");
            context.emit("font-weight", "400");
            context.emit("font-size", "13.333px");
            context.emit("line-height", "1.2");
            context.emit("font-family", "system-ui");
            context.session.result->fontFamilies.push_back("system-ui");
            return;
        }
    }
    // [style || variant || weight || stretch]? size [/ line-height]? family
    std::string style = "normal", weight = "normal";
    std::vector<std::string> dropped;
    size_t i = 0;
    for (; i < value.size(); i++){
        const ComponentValue& v = value[i];
        if (v.isWhitespace())
            continue;
        const std::string k = identOf(v);
        if (k == "normal")
            continue;
        if (k == "italic" || k == "oblique"){
            style = "italic";
            // oblique <angle>
            size_t j = i + 1;
            while (j < value.size() && value[j].isWhitespace())
                j++;
            if (k == "oblique" && j < value.size() && value[j].isToken(TokenType::Dimension) &&
                unitCategory(value[j].token.unit) == UnitCategory::Angle)
                i = j;
            continue;
        }
        if (isWeightKeyword(k)){
            weight = k;
            continue;
        }
        if (v.isToken(TokenType::Number) && v.token.number >= 1 && v.token.number <= 1000){
            weight = text(v);
            continue;
        }
        if (k == "small-caps" || isStretchKeyword(k)){
            dropped.push_back(k);
            continue;
        }
        break;
    }
    // size
    std::vector<ComponentValue> size, lineHeight, family;
    if (i < value.size())
        size.push_back(value[i++]);
    while (i < value.size() && value[i].isWhitespace())
        i++;
    if (i < value.size() && value[i].isToken(TokenType::Delim) && value[i].token.isDelim('/')){
        i++;
        while (i < value.size() && value[i].isWhitespace())
            i++;
        if (i < value.size())
            lineHeight.push_back(value[i++]);
    }
    for (; i < value.size(); i++)
        family.push_back(value[i]);
    family = trimWhitespace(family);
    if (size.empty() || family.empty()){
        unsupportedValue(context, "write font: [style] [weight] <size>[/<line-height>] <family>");
        return;
    }
    if (!dropped.empty())
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + join(dropped, ", ") + "' in the font shorthand is dropped (no font-variant / font-stretch)");
    processSynthetic(context, "font-style", style);
    processSynthetic(context, "font-weight", weight);
    processSynthetic(context, "font-size", text(size));
    processSynthetic(context, "line-height", lineHeight.empty() ? std::string("normal") : text(lineHeight));
    processSynthetic(context, "font-family", text(family));
}
