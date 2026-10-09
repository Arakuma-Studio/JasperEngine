// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lists (phase-2 §3.9.7 "Lists"): list-style-* are properties the compat runtime registers
// (markers drawn by the li and summary elements, §3.11.6); the shorthand is split into them.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssSerialize.h"
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

    bool isEmulatedType(const std::string& k){
        static const std::set<std::string> types = {"disc", "circle", "square", "decimal", "decimal-leading-zero", "lower-alpha",
                                                    "lower-latin", "upper-alpha", "upper-latin", "lower-roman", "upper-roman",
                                                    "lower-greek", "disclosure-open", "disclosure-closed", "none"};
        return types.count(k) > 0;
    }

    // counter styles of CSS Counter Styles 3 the markers do not draw: numeric ones -> decimal
    bool isNumericCounterStyle(const std::string& k){
        static const std::set<std::string> numeric = {
            "arabic-indic", "armenian", "upper-armenian", "lower-armenian", "bengali", "cambodian", "khmer", "cjk-decimal",
            "devanagari", "georgian", "gujarati", "gurmukhi", "hebrew", "kannada", "lao", "malayalam", "mongolian", "myanmar",
            "oriya", "persian", "tamil", "telugu", "thai", "tibetan", "cjk-earthly-branch", "cjk-heavenly-stem", "hiragana",
            "hiragana-iroha", "katakana", "katakana-iroha", "japanese-informal", "japanese-formal", "korean-hangul-formal",
            "korean-hanja-informal", "korean-hanja-formal", "simp-chinese-informal", "simp-chinese-formal",
            "trad-chinese-informal", "trad-chinese-formal", "cjk-ideographic", "ethiopic-numeric"};
        return numeric.count(k) > 0;
    }

    // the list-style-type value written, or "" when not a type
    std::string typeOf(DeclContext& context, const ComponentValue& v, bool report){
        if (v.isToken(TokenType::String))
            return serialize(v);
        const std::string k = identOf(v);
        if (k.empty())
            return std::string();
        if (isEmulatedType(k))
            return k;
        if (isNumericCounterStyle(k) || k == "symbols"){
            if (report)
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "list-style-type '" + k + "' is drawn as decimal");
            return "decimal";
        }
        return std::string();
    }

}

void jasperengine::htmlui::compat::rewriteListStyle(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isVar(w[0]) && n != "list-style"){
        context.emit(n, text(w[0]));
        return;
    }
    if (n == "list-style-type"){
        if (w.size() != 1){
            unsupportedValue(context);
            return;
        }
        std::string type = typeOf(context, w[0], true);
        if (type.empty() && w[0].isToken(TokenType::Ident)){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "list-style-type '" + identOf(w[0]) + "' is drawn as disc");
            type = "disc";
        }
        if (type.empty()){
            unsupportedValue(context, "use disc, circle, square, decimal or a string");
            return;
        }
        context.emit("list-style-type", type);
        return;
    }
    if (n == "list-style-position"){
        const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
        if (k != "inside" && k != "outside"){
            unsupportedValue(context, "use inside or outside");
            return;
        }
        if (k == "inside")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'list-style-position: inside' draws the marker outside and shifts the content");
        context.emit("list-style-position", k);
        return;
    }
    if (n == "list-style-image"){
        if (w.size() == 1 && isIdent(w[0], "none")){
            context.emit("list-style-image", "none");
            return;
        }
        if (w.size() == 1 && urlOf(w[0])){
            const std::string path = resolveUrl(context, w[0], "image");
            if (!path.empty())
                context.emit("list-style-image", path);
            return;
        }
        unsupportedValue(context, "use url(...) or none");
        return;
    }
    // the shorthand: <type> || <position> || <image>; a single none is the type (and image)
    std::string type, position, image;
    int nones = 0;
    for (const ComponentValue& v : w){
        const std::string k = identOf(v);
        if (k == "none"){
            nones++;
            continue;
        }
        if (k == "inside" || k == "outside"){
            position = k;
            continue;
        }
        if (urlOf(v) && !v.isToken(TokenType::String)){
            image = resolveUrl(context, v, "image");
            continue;
        }
        std::string t = typeOf(context, v, true);
        if (t.empty() && v.isToken(TokenType::Ident)){
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "list-style-type '" + k + "' is drawn as disc");
            t = "disc";
        }
        if (t.empty()){
            unsupportedValue(context, "write list-style: <type> <position> <image>");
            return;
        }
        type = t;
    }
    if (nones > 0 && type.empty())
        type = "none";
    if (type.empty())
        type = "disc";
    if (position == "inside")
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'list-style-position: inside' draws the marker outside and shifts the content");
    context.emit("list-style-type", type);
    context.emit("list-style-position", position.empty() ? "outside" : position);
    context.emit("list-style-image", image.empty() ? "none" : image);
}
