// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Small rewrites (phase-2 §3.9.7 "UI", "Images", "SVG paint", Appendix A): colors, cursor,
// pointer-events, focus/navigation properties, visibility, z-index, opacity, accent-color,
// appearance, image-rendering, object-fit, SVG paint, scrollbar styling, content, all,
// direction.

#include "htmlui/compat/css/props/Rewriters.h"

#include "htmlui/compat/css/CascadeKeywords.h"
#include "htmlui/compat/css/Companions.h"
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

    // emits name: value when RCSS accepts it, else reports
    void emitChecked(DeclContext& context, const std::string& property, const std::string& value, const std::string& suggestion = std::string()){
        if (!RcssGrammar::accepts(property, value)){
            unsupportedValue(context, suggestion);
            return;
        }
        context.emit(property, value);
    }

    // the UA's control looks (`appearance: auto`); the user-agent stylesheet reads each with
    // the same default as fallback
    constexpr const char* kCheckLayer =
        "html-layer(\"/@htmlui/ua/check.svg\" 100% 100% 50% 50% no-repeat no-repeat padding-box border-box scroll)";
    constexpr const char* kSelectArrowLayer =
        "html-layer(\"/@htmlui/ua/arrow-down.svg\" 100% 100% 50% 50% no-repeat no-repeat content-box border-box scroll)";
    constexpr const char* kThumbLayer = "linear-gradient(var(--html-accent, #0075ffff), var(--html-accent, #0075ffff))";

}

void jasperengine::htmlui::compat::rewriteColorValue(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
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
    std::vector<std::vector<ComponentValue>> groups = commaGroups(context.value);
    if (groups.empty())
        return;
    if (groups.size() > 1)
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "url() cursors are not supported; the fallback keyword '" +
                         text(groups.back()) + "' is used");
    const std::vector<ComponentValue> last = words(groups.back());
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
    std::vector<ComponentValue> w = words(context.value);
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
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "collapse")){
        context.emit("visibility", "hidden");
        return;
    }
    emitChecked(context, "visibility", context.valueText(), "use visible or hidden");
}

void jasperengine::htmlui::compat::rewriteZIndex(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Number) && !w[0].token.isInteger && std::floor(w[0].token.number) != w[0].token.number){
        unsupportedValue(context, "z-index takes an integer");
        return;
    }
    emitChecked(context, "z-index", context.valueText(), "use auto or an integer");
}

void jasperengine::htmlui::compat::rewriteOpacity(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && w[0].isToken(TokenType::Percentage)){
        context.emit(context.name, formatNumber(w[0].token.number / 100.0));
        return;
    }
    emitChecked(context, context.name, context.valueText(), "use a number between 0 and 1");
}

void jasperengine::htmlui::compat::rewriteAccentColor(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "auto")){
        context.emit(companions::kAccent, "#0075ffff");
        return;
    }
    if (w.size() != 1 || !(isColor(context.session, w[0]) || isVar(w[0]))){
        unsupportedValue(context, "write a CSS color or auto");
        return;
    }
    context.emit(companions::kAccent, text(w[0]));
}

void jasperengine::htmlui::compat::rewriteAppearance(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    const std::string keyword = w.size() == 1 ? identOf(w[0]) : std::string();
    if (keyword == "none"){
        context.emit("--html-check", "none");
        context.emit("--html-control-bg", "transparent");
        context.emit("--html-control-border", "transparent");
        context.emit("--html-select-arrow", "none");
        context.emit("--html-thumb", "none");
        return;
    }
    if (keyword == "auto" || keyword == "menulist" || keyword == "button" || keyword == "textfield" || keyword == "checkbox" ||
        keyword == "radio" || keyword == "listbox" || keyword == "searchfield" || keyword == "menulist-button" ||
        keyword == "slider-horizontal" || keyword == "push-button" || keyword == "square-button"){
        if (keyword != "auto")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'appearance: " + keyword + "' is treated as 'auto'");
        context.emit("--html-check", kCheckLayer);
        context.emit("--html-control-bg", "#ffffffff");
        context.emit("--html-control-border", "#767676ff");
        context.emit("--html-select-arrow", kSelectArrowLayer);
        context.emit("--html-thumb", kThumbLayer);
        return;
    }
    unsupportedValue(context, "use appearance: none or auto");
}

void jasperengine::htmlui::compat::rewriteImageRendering(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
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
    std::vector<ComponentValue> w = words(context.value);
    std::vector<std::string> parts;
    for (const ComponentValue& v : w)
        parts.push_back(v.isToken(TokenType::Number) ? lengthText(v) : text(v));
    emitChecked(context, "object-position", join(parts), "use two values, for example 50% 50% or left top");
}

void jasperengine::htmlui::compat::rewriteSvgPaint(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
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
    std::vector<ComponentValue> w = words(context.value);
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

void jasperengine::htmlui::compat::rewriteContent(DeclContext& context){
    bool pseudoElement = false;
    if (context.rule){
        for (const RewrittenSelector& s : context.rule->rewritten){
            if (s.pseudoElement)
                pseudoElement = true;
        }
    }
    if (context.fork(ForkFeature::PseudoElements) && pseudoElement){
        context.emit("content", context.valueText());
        return;
    }
    context.reportAs(Severity::Info, codes::IgnoredProperty, "'content' only applies to ::before / ::after; dropped");
    context.dropped = true;
}

void jasperengine::htmlui::compat::rewriteAll(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
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
    std::vector<ComponentValue> w = words(context.value);
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
