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
#include <iterator>
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
                // the look itself: HtmlInputElement and HtmlButtonElement read it (HasNativeAppearance)
                {"--html-appearance", "auto"},
                {"--html-check-baseline", "-3px"},
                {"--html-control-bg", "#ffffff"},
                {"--html-control-border", "#767676"},
                {"--html-field-padding", "2px 3px"},
                {"--html-field-border-width", "1px"},
                {"--html-field-border-style", "solid"},
                {"--html-control-radius", "2px"},
                {"--html-check-fill", "var(--html-accent, #0075ff)"},
                {"--html-control-disabled-border", "#bababaab"},
                {"--html-check-disabled-fill", "#c5c5c5c6"},
                {"--html-select-arrow", "url(\"/@htmlui/ua/arrow-down.svg\")"},
                {"--html-select-arrow-color", "currentcolor"},
                {"--html-thumb", "none"},
                {"--html-slider-fill", "var(--html-accent, #0075ff)"},
                {"--html-slider-height", "16px"},
                {"--html-slider-track-margin", "4px"},
                {"--html-slider-track-inset", "1px"},
                {"--html-slider-thumb-margin", "-2px"},
                {"--html-slider-thumb-transform", "scale(0.9375)"},
                {"--html-slider-track-height", "6px"},
                {"--html-slider-track-border", "1px"},
                // the track's own colors (not the checkbox's and select's --html-control-*)
                {"--html-slider-track-bg", "#efefef"},
                {"--html-slider-track-border-color", "#b2b2b2"},
                // checkbox and radio: the theme's size (p4-gaps2)
                {"--html-check-size", "13px"},
                // selects, buttons and progress bars have their own (p4-gaps2): html.css keeps
                // other borders and backgrounds for them than for checkboxes
                {"--html-select-bg", "#ffffff"},
                {"--html-select-padding", "1px 0 1px 4px"},
                {"--html-select-arrow-width", "16px"},
                {"--html-button-padding", "2px 7px"},
                {"--html-button-border-width", "1px"},
                {"--html-button-border-style", "solid"},
                {"--html-button-border", "#767676"},
                {"--html-button-hover-bg", "#e5e5e5"},
                {"--html-button-hover-border", "#4f4f4f"},
                {"--html-button-active-bg", "#f5f5f5"},
                {"--html-button-active-border", "#8d8d8d"},
                {"--html-progress-border-width", "0.25em"},
                {"--html-progress-radius", "0.5em"},
                {"--html-progress-bg", "#00000000"},
                {"--html-progress-track", "inset 0 0 0 1px #b2b2b2, inset 0 0 0 1em #efefef"},
                {"--html-progress-fill-radius", "0.25em 0 0 0.25em"},
                {"--html-progress-fill", "var(--html-accent, #0075ff)"},
                {"--html-progress-fill-line", "inset 0 1px 0 0 #76767680, inset 0 -1px 0 0 #76767680, inset 1px 0 0 0 #76767680"},
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
    context.emit("--html-accent-contrast", light ? "#3b3b3bff" : "#ffffffff");
}

void jasperengine::htmlui::compat::rewriteAppearance(DeclContext& context){
    ValueList w = words(context.value);
    const std::string keyword = w.size() == 1 ? identOf(w[0]) : std::string();
    if (keyword == "none"){
        // (a checkbox or radio without the look draws no theme: HtmlInputElement)
        context.emit("--html-appearance", "none");
        context.emit("--html-check-baseline", "0px");
        // checkbox and radio: sized by their content (::before/::after, padding, border), not 13x13
        context.emit("--html-control-bg", "transparent");
        context.emit("--html-control-border", "transparent");
        context.emit("--html-check-size", "auto");
        // text fields, buttons, selects: the theme's 2px rounding (Chromium's html.css has none)
        context.emit("--html-control-radius", "0px");
        // text fields: html.css's `padding: 1px 2px; border: 2px inset` (the fork draws inset: BorderStyle, p3-paint)
        context.emit("--html-field-padding", "1px 2px");
        context.emit("--html-field-border-width", "2px");
        context.emit("--html-field-border-style", context.fork(ForkFeature::BorderStyle) ? "inset" : "solid");
        context.emit("--html-check-fill", "transparent");
        context.emit("--html-control-disabled-border", "transparent");
        context.emit("--html-check-disabled-fill", "transparent");
        // select: html.css's 1px #767676 border on ButtonFace, no arrow, none of the menu list's
        // inner padding (Chromium)
        context.emit("--html-select-bg", "#efefef");
        context.emit("--html-select-padding", "0px");
        context.emit("--html-select-arrow-width", "0px");
        context.emit("--html-select-arrow", "none");
        context.emit("--html-select-arrow-color", "transparent");
        // buttons: html.css's `padding: 1px 6px; border: 2px outset ButtonBorder` (black), the same
        // on hover and press (the fork draws outset: BorderStyle, p3-paint)
        context.emit("--html-button-padding", "1px 6px");
        context.emit("--html-button-border-width", "2px");
        context.emit("--html-button-border-style", context.fork(ForkFeature::BorderStyle) ? "outset" : "solid");
        context.emit("--html-button-border", "#000000");
        context.emit("--html-button-hover-bg", "#efefef");
        context.emit("--html-button-hover-border", "#000000");
        context.emit("--html-button-active-bg", "#efefef");
        context.emit("--html-button-active-border", "#000000");
        // progress: html.css's ::-webkit-progress-bar (gray, the whole box) and -value (green)
        context.emit("--html-progress-border-width", "0px");
        context.emit("--html-progress-radius", "0px");
        context.emit("--html-progress-bg", "#808080");
        context.emit("--html-progress-track", "0 0 0 0 #0000");
        context.emit("--html-progress-fill-radius", "0px");
        context.emit("--html-progress-fill", "#008000");
        context.emit("--html-progress-fill-line", "0 0 0 0 #0000");
        context.emit("--html-thumb", "none");
        // a range without its look: no value fill, as tall as its track (Chromium)
        context.emit("--html-slider-fill", "transparent");
        context.emit("--html-slider-height", "auto");
        context.emit("--html-slider-track-margin", "0px");
        context.emit("--html-slider-track-inset", "0px");
        context.emit("--html-slider-thumb-margin", "0px");
        context.emit("--html-slider-thumb-transform", "none");
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
        for (const char* companion : {"--html-appearance", "--html-check-baseline", "--html-control-bg", "--html-control-border",
                                      "--html-field-padding", "--html-field-border-width",
                                      "--html-field-border-style", "--html-control-radius", "--html-check-fill",
                                      "--html-control-disabled-border", "--html-check-disabled-fill",
                                      "--html-select-arrow", "--html-select-arrow-color", "--html-thumb", "--html-slider-fill", "--html-slider-height",
                                      "--html-slider-track-margin", "--html-slider-track-inset", "--html-slider-thumb-margin", "--html-slider-thumb-transform",
                                      "--html-slider-track-height", "--html-slider-track-border",
                                      "--html-slider-track-bg", "--html-slider-track-border-color", "--html-check-size", "--html-select-bg",
                                      "--html-select-padding", "--html-select-arrow-width", "--html-button-padding", "--html-button-border-width",
                                      "--html-button-border-style", "--html-button-border", "--html-button-hover-bg", "--html-button-hover-border",
                                      "--html-button-active-bg", "--html-button-active-border", "--html-progress-border-width",
                                      "--html-progress-radius", "--html-progress-bg", "--html-progress-track", "--html-progress-fill-radius",
                                      "--html-progress-fill", "--html-progress-fill-line"})
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
    if (context.fork(ForkFeature::ObjectFit)){
        // the fork takes the CSS <position> (edge offsets: 'right 10px bottom 20%'; phase 3, p3-paint)
        if (parts.empty() || parts.size() > 4){
            unsupportedValue(context, "use one to four values, for example 50% 50%, left top or right 10px bottom 20px");
            return;
        }
        context.emit("object-position", join(parts));
        return;
    }
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

namespace {
    // counter(name, style) / counters(name, separator, style) with a counter style the fork does
    // not draw: it counts in decimal (phase-3 spec 2.2), said once per declaration
    void reportCounterStyles(DeclContext& context){
        static const char* const known[] = {"decimal", "decimal-leading-zero", "lower-alpha", "lower-latin", "upper-alpha",
                                            "upper-latin", "lower-roman", "upper-roman", "lower-greek", "disc", "circle",
                                            "square", "none", "disclosure-open", "disclosure-closed"};
        for (const ComponentValue& v : context.value){
            const bool counters = v.isFunction("counters");
            if (!counters && !v.isFunction("counter"))
                continue;
            const std::vector<ValueList> arguments = commaGroups(v.children);
            const size_t styleIndex = counters ? 2 : 1;
            if (arguments.size() <= styleIndex)
                continue;
            const ValueList styleWords = words(arguments[styleIndex]);
            const std::string style = styleWords.size() == 1 ? identOf(styleWords[0]) : std::string();
            if (style.empty())
                continue;   // a string, or nothing the fork reads: its own parser decides
            bool found = false;
            for (const char* name : known)
                found = found || style == name;
            if (!found){
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "counter style '" + style + "' is drawn as decimal",
                                 "use one of: decimal, decimal-leading-zero, lower/upper-alpha/latin, lower/upper-roman, lower-greek, disc, circle, square, none");
                return;
            }
        }
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
        reportCounterStyles(context);
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
    // the fork's 'all' (phase 3, p3-cascade): every property it has, inherit and revert as CSS says (not direction, nor
    // custom properties, nor RmlUi's interaction properties)
    if (context.fork(ForkFeature::CssWideKeywords)){
        context.emit("all", keyword);
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
    // ... and the paint properties the fork has (phase 3, p3-paint)
    std::vector<const char*> list(std::begin(properties), std::end(properties));
    if (context.fork(ForkFeature::BorderStyle))
        list.insert(list.end(), {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"});
    if (context.fork(ForkFeature::Outline))
        list.insert(list.end(), {"outline-width", "outline-style", "outline-color", "outline-offset"});
    if (context.fork(ForkFeature::BackgroundLayers))
        list.insert(list.end(), {"background-position-x", "background-position-y", "background-size", "background-repeat",
                                 "background-origin", "background-clip", "background-attachment", "mask-position-x",
                                 "mask-position-y", "mask-size", "mask-repeat"});
    if (context.fork(ForkFeature::ObjectFit))
        list.insert(list.end(), {"object-fit", "object-position"});
    if (context.fork(ForkFeature::GroupOpacity))
        list.push_back("isolation");
    for (const char* property : list){
        const PropertySupport* row = findProperty(property);
        if (keyword == "inherit" && !(row && row->inherited))
            continue;
        processSynthetic(context, property, keyword == "revert" || keyword == "revert-layer" ? "unset" : keyword);
    }
}

void jasperengine::htmlui::compat::rewriteDirection(DeclContext& context){
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    // phase-3 spec 2.2: the text runs right to left, the layout is not mirrored
    if (k == "rtl")
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'direction: rtl' sets the text direction; block, flex and grid layouts are not mirrored",
                         hints::kMirroredLayout);
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
