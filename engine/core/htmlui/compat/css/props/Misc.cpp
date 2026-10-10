// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Small rewrites (phase-2 §3.9.7 "UI", "Images", "SVG paint", Appendix A): colors, cursor,
// pointer-events, focus/navigation properties, visibility, z-index, opacity, accent-color,
// appearance, image-rendering, object-fit, SVG paint, scrollbar styling, content, all,
// direction.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CascadeKeywords.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include <cmath>
#include <map>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    // emits name: value when RCSS accepts it, else reports
    void emitChecked(DeclContext& context, const std::string& property, const std::string& value, const std::string& suggestion = std::string()){
        if (!RcssGrammar::accepts(property, value, context.session.options.fork)){
            unsupportedValue(context, suggestion);
            return;
        }
        context.emit(property, value);
    }

    // the UA's control looks (`appearance: auto`): the fallbacks the user-agent stylesheet
    // gives its var(--html-…) reads, taken from its source (3.9.7 "UI"); these when it has
    // none (a build without the embedded sheet)
    const std::string& uaCompanionDefault(const std::string& name){
        static const std::map<std::string, std::string> defaults = [](){
            std::map<std::string, std::string> out = {
                {"--html-check", "html-layer(\"/@htmlui/ua/check.svg\" 100% 100% 50% 50% no-repeat no-repeat padding-box border-box scroll)"},
                {"--html-check-baseline", "-3px"},
                {"--html-control-bg", "#ffffff"},
                {"--html-control-border", "#767676"},
                {"--html-select-arrow", "html-layer(\"/@htmlui/ua/arrow-down.svg\" 10px 6px 3px 50% no-repeat no-repeat padding-box border-box scroll)"},
                {"--html-thumb", "none"},
                {"--html-slider-fill", "var(--html-accent, #0075ff)"},
                {"--html-slider-height", "16px"},
                {"--html-slider-track-margin", "5px"},
                {"--html-slider-track-height", "6px"},
                {"--html-slider-track-border", "1px"},
                // the track's own colors (not the checkbox's and select's --html-control-*)
                {"--html-slider-track-bg", "#efefef"},
                {"--html-slider-track-border-color", "#b2b2b2"},
            };
            const std::string_view ua = UserAgentStyle::source();
            for (auto& entry : out){
                const std::string needle = "var(" + entry.first + ",";
                const size_t at = ua.find(needle);
                if (at == std::string_view::npos)
                    continue;
                // the fallback runs to the var()'s closing parenthesis
                const size_t start = at + needle.size();
                int depth = 1;
                bool quote = false;
                size_t i = start;
                for (; i < ua.size(); i++){
                    const char c = ua[i];
                    if (quote){
                        if (c == '\\')
                            i++;
                        else if (c == '"')
                            quote = false;
                        continue;
                    }
                    if (c == '"')
                        quote = true;
                    else if (c == '(')
                        depth++;
                    else if (c == ')' && --depth == 0)
                        break;
                }
                if (depth != 0)
                    continue;
                std::string_view fallback = ua.substr(start, i - start);
                while (!fallback.empty() && (fallback.front() == ' ' || fallback.front() == '\n'))
                    fallback.remove_prefix(1);
                while (!fallback.empty() && (fallback.back() == ' ' || fallback.back() == '\n'))
                    fallback.remove_suffix(1);
                if (!fallback.empty())
                    entry.second = std::string(fallback);
            }
            // colors as the rewriter writes them (#rrggbbaa)
            for (auto& entry : out){
                ColorParseInfo info;
                if (std::optional<CssColor> color = ColorValue::parse(entry.second, &info))
                    entry.second = ColorValue::hex(ColorValue::toRgba8(*color));
            }
            return out;
        }();
        static const std::string empty;
        const auto it = defaults.find(name);
        return it == defaults.end() ? empty : it->second;
    }

}

void jasperengine::htmlui::compat::rewriteColorValue(DeclContext& context){
    ValueList w = words(context.value);
    if (context.name == "caret-color" && w.size() == 1 && isIdent(w[0], "auto")){
        context.emit("caret-color", "auto");
        return;
    }
    if (w.size() != 1 || !(isColor(context.session, w[0]) || isVar(w[0]))){
        unsupportedValue(context, "write a CSS color");
        return;
    }
    context.emit(context.name, text(w[0]));
}

void jasperengine::htmlui::compat::rewriteCursor(DeclContext& context){
    std::vector<ValueList> groups = commaGroups(context.value);
    if (groups.empty())
        return;
    if (groups.size() > 1)
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "url() cursors are not supported; the fallback keyword '" +
                         text(groups.back()) + "' is used");
    const ValueList last = words(groups.back());
    if (last.size() != 1 || !last[0].isToken(TokenType::Ident)){
        if (last.size() == 1 && isVar(last[0])){
            context.emit("cursor", text(last[0]));
            return;
        }
        unsupportedValue(context, "use a cursor keyword such as pointer");
        return;
    }
    const std::string keyword = identOf(last[0]);
    if (!findValue("cursor", keyword)){
        unsupportedValue(context, "use a CSS cursor keyword such as pointer, default or text");
        return;
    }
    context.emit("cursor", keyword);
}

void jasperengine::htmlui::compat::rewritePointerEvents(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Ident)){
        const std::string k = identOf(w[0]);
        if (k == "auto" || k == "none"){
            context.emit("pointer-events", k);
            return;
        }
        if (const ValueSupport* value = findValue("pointer-events", w[0].token.value)){
            context.emit("pointer-events", value->output);
            return;
        }
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("pointer-events", text(w[0]));
        return;
    }
    unsupportedValue(context, "use auto or none");
}

void jasperengine::htmlui::compat::rewriteTabIndexLike(DeclContext& context){
    emitChecked(context, context.name, context.valueText());
}

void jasperengine::htmlui::compat::rewriteVisibility(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "collapse")){
        context.emit("visibility", "hidden");
        return;
    }
    emitChecked(context, "visibility", context.valueText(), "use visible or hidden");
}

void jasperengine::htmlui::compat::rewriteZIndex(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Number) && !w[0].token.isInteger && std::floor(w[0].token.number) != w[0].token.number){
        unsupportedValue(context, "z-index takes an integer");
        return;
    }
    emitChecked(context, "z-index", context.valueText(), "use auto or an integer");
}

void jasperengine::htmlui::compat::rewriteOpacity(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Percentage)){
        context.emit(context.name, formatNumber(w[0].token.number / 100.0));
        return;
    }
    emitChecked(context, context.name, context.valueText(), "use a number between 0 and 1");
}

void jasperengine::htmlui::compat::rewriteAccentColor(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "auto")){
        context.emit(companions::kAccent, "#0075ffff");
        return;
    }
    if (w.size() != 1 || !(isColor(context.session, w[0]) || isVar(w[0]))){
        unsupportedValue(context, "write a CSS color or auto");
        return;
    }
    context.emit(companions::kAccent, text(w[0]));

    // Chromium draws the check mark and the checked radio's gap dark (#3b3b3b) on a light accent
    // (relative luminance above ~0.32, measured on Chromium 153), white otherwise
    ValueList color = w;
    if (isVar(w[0])){
        ValueList replacement;
        if (!context.session.staticVar(w[0], replacement))
            return;   // set at runtime: the user-agent's white mark
        color = words(replacement);
    }
    const std::string hex = color.size() == 1 ? text(color[0]) : std::string();
    if (hex.size() < 7 || hex[0] != '#')
        return;
    const auto channel = [&](size_t at){
        const double c = std::strtol(hex.substr(at, 2).c_str(), nullptr, 16) / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5);
    const bool light = luminance > 0.32;
    context.emit("--html-check", std::string("html-layer(\"/@htmlui/ua/") + (light ? "check-dark.svg" : "check.svg") +
                                 "\" 100% 100% 50% 50% no-repeat no-repeat padding-box border-box scroll)");
    context.emit("--html-accent-contrast", light ? "#3b3b3bff" : "#ffffffff");
}

void jasperengine::htmlui::compat::rewriteAppearance(DeclContext& context){
    ValueList w = words(context.value);
    const std::string keyword = w.size() == 1 ? identOf(w[0]) : std::string();
    if (keyword == "none"){
        context.emit("--html-check", "none");
        context.emit("--html-check-baseline", "0px");
        context.emit("--html-control-bg", "transparent");
        context.emit("--html-control-border", "transparent");
        context.emit("--html-select-arrow", "none");
        context.emit("--html-thumb", "none");
        // a range without its look: no value fill, as tall as its track (Chromium)
        context.emit("--html-slider-fill", "transparent");
        context.emit("--html-slider-height", "auto");
        context.emit("--html-slider-track-margin", "0px");
        context.emit("--html-slider-track-height", "auto");
        context.emit("--html-slider-track-border", "0px");
        context.emit("--html-slider-track-bg", "transparent");
        context.emit("--html-slider-track-border-color", "transparent");
        return;
    }
    if (keyword == "auto" || keyword == "menulist" || keyword == "button" || keyword == "textfield" || keyword == "checkbox" ||
        keyword == "radio" || keyword == "listbox" || keyword == "searchfield" || keyword == "menulist-button" ||
        keyword == "slider-horizontal" || keyword == "push-button" || keyword == "square-button"){
        if (keyword != "auto")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'appearance: " + keyword + "' is treated as 'auto'");
        for (const char* companion : {"--html-check", "--html-check-baseline", "--html-control-bg", "--html-control-border",
                                      "--html-select-arrow", "--html-thumb", "--html-slider-fill", "--html-slider-height",
                                      "--html-slider-track-margin", "--html-slider-track-height", "--html-slider-track-border",
                                      "--html-slider-track-bg", "--html-slider-track-border-color"})
            context.emit(companion, uaCompanionDefault(companion));
        return;
    }
    unsupportedValue(context, "use appearance: none or auto");
}

void jasperengine::htmlui::compat::rewriteImageRendering(DeclContext& context){
    ValueList w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Ident)){
        std::string k = identOf(w[0]);
        if (k == "optimizespeed" || k == "-webkit-optimize-contrast" || k == "-moz-crisp-edges")
            k = "crisp-edges";
        else if (k == "optimizequality")
            k = "smooth";
        emitChecked(context, "image-rendering", k, "use pixelated, crisp-edges or auto");
        return;
    }
    emitChecked(context, "image-rendering", context.valueText(), "use pixelated, crisp-edges or auto");
}

void jasperengine::htmlui::compat::rewriteObjectFit(DeclContext& context){
    if (context.name == "object-fit"){
        emitChecked(context, "object-fit", context.valueText(), "use fill, contain, cover, none or scale-down");
        return;
    }
    // object-position: up to two values (keywords, lengths, percentages)
    ValueList w = words(context.value);
    std::vector<std::string> parts;
    for (const ComponentValue& v : w)
        parts.push_back(v.isToken(TokenType::Number) ? lengthText(v) : text(v));
    emitChecked(context, "object-position", join(parts), "use two values, for example 50% 50% or left top");
}

void jasperengine::htmlui::compat::rewriteSvgPaint(DeclContext& context){
    ValueList w = words(context.value);
    std::string value = context.valueText();
    if (context.name == "stroke-dasharray"){
        // registered as a string: the list as written
        context.emit(context.name, value);
        return;
    }
    if ((context.name == "fill" || context.name == "stroke") && w.size() == 1 && w[0].isFunction("url")){
        context.reportAs(Severity::Info, codes::UnsupportedValue, "paint servers (url()) are not supported on the <svg> element; dropped",
                         "set the gradient inside the SVG");
        context.dropped = true;
        return;
    }
    emitChecked(context, context.name, value);
}

void jasperengine::htmlui::compat::rewriteScrollbarStyle(DeclContext& context){
    if (context.keyframe)
        return;
    ValueList w = words(context.value);
    if (context.name == "scrollbar-width"){
        const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
        if (k == "auto")
            return;
        std::string size;
        if (k == "none")
            size = "0px";
        else if (k == "thin")
            size = "8px";
        else if (w.size() == 1 && isLength(context.session, w[0]))
            size = lengthText(w[0]);
        else{
            unsupportedValue(context, "use auto, thin or none");
            return;
        }
        context.generate(" > scrollbarvertical", "width", size);
        context.generate(" > scrollbarhorizontal", "height", size);
        return;
    }
    // scrollbar-color: <thumb> <track>
    if (w.size() == 1 && isIdent(w[0], "auto"))
        return;
    if (w.size() != 2 || !isColor(context.session, w[0]) || !isColor(context.session, w[1])){
        unsupportedValue(context, "write two colors: thumb and track");
        return;
    }
    const std::string thumb = text(w[0]), track = text(w[1]);
    context.generate(" > scrollbarvertical > sliderbar", "background-color", thumb);
    context.generate(" > scrollbarhorizontal > sliderbar", "background-color", thumb);
    context.generate(" > scrollbarvertical > slidertrack", "background-color", track);
    context.generate(" > scrollbarhorizontal > slidertrack", "background-color", track);
}

namespace {
    std::string trimText(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\n");
        if (first == std::string::npos)
            return std::string();
        return text.substr(first, text.find_last_not_of(" \t\n") - first + 1);
    }
}

void jasperengine::htmlui::compat::rewriteContent(DeclContext& context){
    bool pseudoElement = false;
    if (context.rule){
        for (const RewrittenSelector& s : context.rule->rewritten){
            if (s.pseudoElement)
                pseudoElement = true;
        }
    }
    if (context.fork(ForkFeature::PseudoElements) && pseudoElement){
        // url() items point at the resolved resource, as in every other image property; the rest
        // (strings, attr(), counter(), counters(), quotes) is the fork's as written
        ValueList w = words(context.value);
        bool hasUrl = false;
        for (const ComponentValue& v : w)
            hasUrl = hasUrl || (urlOf(v) && !v.isToken(TokenType::String));
        if (!hasUrl){
            context.emit("content", context.valueText());
            return;
        }
        std::string value;
        for (const ComponentValue& v : context.value){
            if (urlOf(v) && !v.isToken(TokenType::String)){
                const std::string path = resolveUrl(context, v, "image");
                if (path.empty())
                    return;   // reported
                value += "url(" + serializeString(path) + ")";
                continue;
            }
            appendSerialized(value, v);
        }
        context.emit("content", trimText(value));
        return;
    }
    context.reportAs(Severity::Info, codes::IgnoredProperty, "'content' only applies to ::before / ::after; dropped");
    context.dropped = true;
}

void jasperengine::htmlui::compat::rewriteAll(DeclContext& context){
    ValueList w = words(context.value);
    const std::string keyword = w.size() == 1 ? identOf(w[0]) : std::string();
    if (keyword != "unset" && keyword != "initial" && keyword != "inherit" && keyword != "revert" && keyword != "revert-layer"){
        unsupportedValue(context, "use all: unset");
        return;
    }
    if (keyword == "inherit")
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'all: inherit' resets the inherited properties only");
    // every CSS property the engine has (not direction, unicode-bidi or custom properties)
    static const char* const properties[] = {
        "margin-top", "margin-right", "margin-bottom", "margin-left", "padding-top", "padding-right", "padding-bottom",
        "padding-left", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "border-top-left-radius",
        "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius", "display", "position", "top",
        "right", "bottom", "left", "float", "clear", "box-sizing", "z-index", "width", "min-width", "max-width", "height",
        "min-height", "max-height", "line-height", "vertical-align", "overflow-x", "overflow-y", "visibility", "text-overflow",
        "background-color", "color", "caret-color", "opacity", "font-family", "font-style", "font-weight", "font-size",
        "letter-spacing", "text-align", "text-decoration", "text-transform", "white-space", "word-break", "row-gap",
        "column-gap", "cursor", "pointer-events", "perspective", "transform", "transition", "animation",
        "background-image", "mask-image", "text-shadow", "filter", "backdrop-filter", "box-shadow", "align-content",
        "align-items", "align-self", "flex-basis", "flex-direction", "flex-grow", "flex-shrink", "flex-wrap",
        "justify-content",
    };
    for (const char* property : properties){
        const PropertySupport* row = findProperty(property);
        if (keyword == "inherit" && !(row && row->inherited))
            continue;
        processSynthetic(context, property, keyword == "revert" || keyword == "revert-layer" ? "unset" : keyword);
    }
}

void jasperengine::htmlui::compat::rewriteDirection(DeclContext& context){
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    if (k == "ltr" || k == "rtl"){
        context.emit("-rmlui-direction", k);
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("-rmlui-direction", text(w[0]));
        return;
    }
    unsupportedValue(context, "use ltr or rtl");
}
