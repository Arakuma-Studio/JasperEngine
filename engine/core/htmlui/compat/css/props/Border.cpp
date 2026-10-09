// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Borders (phase-2 §3.9.7 "Borders", "Border radius", "Border image"): RCSS border shorthands
// take only width and color, so every border declaration becomes per-side width and color
// longhands; styles are emulated until the fork has BorderStyle (none/hidden = width 0, the
// 3D styles shaded the way Chromium shades them, dashed/dotted/double solid); percentages of
// radii until PercentRadius; border-image through the html-border-image decorator.

#include "htmlui/compat/css/props/Rewriters.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    const char* const kSides[] = {"top", "right", "bottom", "left"};

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    bool isStyle(const std::string& k){
        return k == "none" || k == "hidden" || k == "dotted" || k == "dashed" || k == "solid" || k == "double" || k == "groove" ||
               k == "ridge" || k == "inset" || k == "outset";
    }

    std::optional<std::string> widthOf(Session& session, const ComponentValue& v){
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

    // 1-4 values to top, right, bottom, left
    std::vector<std::string> boxValues(const std::vector<std::string>& values){
        std::vector<std::string> out(4);
        if (values.empty())
            return {};
        out[0] = values[0];
        out[1] = values.size() > 1 ? values[1] : out[0];
        out[2] = values.size() > 2 ? values[2] : out[0];
        out[3] = values.size() > 3 ? values[3] : out[1];
        return out;
    }

    // Chromium's 3D border shading (Color::Dark / Color::Light, BoxBorderPainter)
    uint32_t shade(uint32_t rgba, bool darken){
        const int r = (int)((rgba >> 24) & 0xFF), g = (int)((rgba >> 16) & 0xFF), b = (int)((rgba >> 8) & 0xFF);
        const uint32_t a = rgba & 0xFF;
        auto pack = [&](int x, int y, int z){ return ((uint32_t)x << 24) | ((uint32_t)y << 16) | ((uint32_t)z << 8) | a; };
        auto distance = [](int x, int y, int z, int c){ return (x - c) * (x - c) + (y - c) * (y - c) + (z - c) * (z - c); };
        const float scale = std::nextafter(256.0f, 0.0f);
        const float v = (float)std::max({r, g, b}) / scale;
        if (darken){
            if (distance(r, g, b, 0) <= distance(0x20, 0x20, 0x20, 0))
                return rgba;
            if (r == 0 && g == 0 && b == 0)
                return pack(0x54, 0x54, 0x54);
            const float m = v == 0.0f ? 0.0f : std::max(0.0f, (v - 0.33f) / v);
            return pack((int)(m * r), (int)(m * g), (int)(m * b));
        }
        if (distance(r, g, b, 255) <= distance(0xEB, 0xEB, 0xEB, 255))
            return rgba;
        if (v == 0.0f)
            return pack(0x54, 0x54, 0x54);
        const float m = std::min(1.0f, v + 0.33f) / v;
        return pack(std::min(255, (int)(m * r)), std::min(255, (int)(m * g)), std::min(255, (int)(m * b)));
    }

    std::optional<uint32_t> hexValue(const std::string& color){
        if (color.size() != 9 || color[0] != '#')
            return std::nullopt;
        return (uint32_t)std::strtoul(color.c_str() + 1, nullptr, 16);
    }

    std::string hexText(uint32_t rgba){
        char buffer[10];
        std::snprintf(buffer, sizeof(buffer), "#%08x", rgba);
        return buffer;
    }

    // the sides a border property names
    std::vector<int> sidesOf(Session& session, const std::string& name){
        const bool rtl = session.rootDirection == "rtl";
        const int inlineStart = rtl ? 1 : 3, inlineEnd = rtl ? 3 : 1;
        if (name.compare(0, 13, "border-inline") == 0){
            if (name.compare(0, 19, "border-inline-start") == 0)
                return {inlineStart};
            if (name.compare(0, 17, "border-inline-end") == 0)
                return {inlineEnd};
            return {3, 1};
        }
        if (name.compare(0, 12, "border-block") == 0){
            if (name.compare(0, 18, "border-block-start") == 0)
                return {0};
            if (name.compare(0, 16, "border-block-end") == 0)
                return {2};
            return {0, 2};
        }
        for (int i = 0; i < 4; i++){
            const std::string prefix = std::string("border-") + kSides[i];
            if (name.compare(0, prefix.size(), prefix) == 0)
                return {i};
        }
        return {0, 1, 2, 3};
    }

    std::string partOf(const std::string& name){
        if (name.size() >= 6 && name.compare(name.size() - 6, 6, "-width") == 0)
            return "width";
        if (name.size() >= 6 && name.compare(name.size() - 6, 6, "-color") == 0)
            return "color";
        if (name.size() >= 6 && name.compare(name.size() - 6, 6, "-style") == 0)
            return "style";
        return "all";
    }

    // the border style of a rule for a side (none when unknown)
    void emitStyle(DeclContext& context, const std::vector<int>& sides, const std::string& style, const std::string& color,
                   const std::string& width, bool widthGiven){
        const bool fork = context.fork(ForkFeature::BorderStyle);
        for (int side : sides){
            const std::string prefix = std::string("border-") + kSides[side];
            std::string sideWidth = width;
            std::string sideColor = color;
            if (fork){
                if (widthGiven || style != "none")
                    context.emit(prefix + "-width", sideWidth);
                context.emit(prefix + "-style", style);
                context.emit(prefix + "-color", sideColor);
                continue;
            }
            if (style == "none" || style == "hidden")
                sideWidth = "0px";
            if (style == "inset" || style == "outset" || style == "groove" || style == "ridge"){
                if (std::optional<uint32_t> rgba = hexValue(color)){
                    const bool topLeft = side == 0 || side == 3;
                    const bool darken = topLeft == (style == "inset" || style == "groove");
                    sideColor = hexText(shade(*rgba, darken));
                }
            }
            context.emit(prefix + "-width", sideWidth);
            context.emit(prefix + "-color", sideColor);
        }
    }

    void reportStyle(DeclContext& context, const std::string& style){
        if (context.fork(ForkFeature::BorderStyle))
            return;
        if (style == "dashed" || style == "dotted" || style == "double")
            context.reportAs(Severity::Warning, codes::ApproximatedVisible, "'border-style: " + style + "' is drawn solid until the fork has BorderStyle",
                             "use solid, or draw the pattern with an image");
        else if (style == "inset" || style == "outset" || style == "groove" || style == "ridge")
            context.reportAs(Severity::Info, codes::ForkFallback, "'border-style: " + style +
                             "' is drawn solid with Chromium's shaded side colors until the fork has BorderStyle");
    }

    bool ruleSetsWidth(const FlatRule& rule, int side){
        for (const Declaration& d : rule.declarations){
            const std::string& n = d.name;
            if (n == "border" || n == "border-width" || n == std::string("border-") + kSides[side] ||
                n == std::string("border-") + kSides[side] + "-width")
                return true;
        }
        return false;
    }

    // ---- border-image

    struct BorderImage {
        std::string source;           // the emitted path; "" = none
        bool gradient = false;
        std::vector<std::string> slice{"100%"};
        bool fill = false;
        std::vector<std::string> width{"1"};
        std::vector<std::string> outset{"0"};
        std::vector<std::string> repeat{"stretch"};
    };

    std::string numberOrLength(const ComponentValue& v){
        if (v.isToken(TokenType::Number))
            return text(v);
        return lengthText(v);
    }

    // applies one border-image declaration; false on a value it cannot read
    bool applyBorderImage(DeclContext& context, const std::string& property, const std::vector<ComponentValue>& value, BorderImage& image){
        std::vector<ComponentValue> w = words(value);
        auto source = [&](const ComponentValue& v) -> bool {
            if (isIdent(v, "none")){
                image.source.clear();
                return true;
            }
            if (urlOf(v)){
                image.source = resolveUrl(context, v, "image");
                return true;
            }
            if (v.kind == ComponentValue::Kind::Function && lowerAscii(v.name()).find("gradient") != std::string::npos){
                image.gradient = true;
                return true;
            }
            return false;
        };
        if (property == "border-image-source")
            return w.size() == 1 && source(w[0]);
        if (property == "border-image-repeat"){
            image.repeat.clear();
            for (const ComponentValue& v : w){
                const std::string k = identOf(v);
                if (k != "stretch" && k != "repeat" && k != "round" && k != "space")
                    return false;
                image.repeat.push_back(k);
            }
            return !image.repeat.empty() && image.repeat.size() <= 2;
        }
        if (property == "border-image-slice"){
            image.slice.clear();
            image.fill = false;
            for (const ComponentValue& v : w){
                if (isIdent(v, "fill"))
                    image.fill = true;
                else if (v.isToken(TokenType::Number) || v.isToken(TokenType::Percentage))
                    image.slice.push_back(text(v));
                else
                    return false;
            }
            return !image.slice.empty() && image.slice.size() <= 4;
        }
        if (property == "border-image-width" || property == "border-image-outset"){
            std::vector<std::string>& target = property == "border-image-width" ? image.width : image.outset;
            target.clear();
            for (const ComponentValue& v : w){
                if (v.isToken(TokenType::Number) || isLength(context.session, v, property == "border-image-width") || isIdent(v, "auto"))
                    target.push_back(isIdent(v, "auto") ? "auto" : numberOrLength(v));
                else
                    return false;
            }
            return !target.empty() && target.size() <= 4;
        }
        // the shorthand: <source> || <slice> [/ <width>? [/ <outset>]?]? || <repeat>
        int section = 0;   // 0 slice, 1 width, 2 outset
        bool sliceStarted = false;
        std::vector<std::string> slice, widths, outsets;
        std::vector<std::string> repeats;
        bool fill = false;
        for (const ComponentValue& v : value){
            if (isWs(v))
                continue;
            if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                if (!sliceStarted)
                    return false;
                section++;
                if (section > 2)
                    return false;
                continue;
            }
            const std::string k = identOf(v);
            if (k == "stretch" || k == "repeat" || k == "round" || k == "space"){
                repeats.push_back(k);
                continue;
            }
            if (k == "fill"){
                fill = true;
                sliceStarted = true;
                continue;
            }
            if (urlOf(v) || k == "none" || (v.kind == ComponentValue::Kind::Function && lowerAscii(v.name()).find("gradient") != std::string::npos)){
                if (!source(v))
                    return false;
                continue;
            }
            if (section == 0 && (v.isToken(TokenType::Number) || v.isToken(TokenType::Percentage))){
                slice.push_back(text(v));
                sliceStarted = true;
                continue;
            }
            if (section == 1 && (v.isToken(TokenType::Number) || isLength(context.session, v, true) || k == "auto")){
                widths.push_back(k == "auto" ? "auto" : numberOrLength(v));
                continue;
            }
            if (section == 2 && (v.isToken(TokenType::Number) || isLength(context.session, v))){
                outsets.push_back(numberOrLength(v));
                continue;
            }
            return false;
        }
        image.slice = slice.empty() ? std::vector<std::string>{"100%"} : slice;
        image.fill = fill;
        image.width = widths.empty() ? std::vector<std::string>{"1"} : widths;
        image.outset = outsets.empty() ? std::vector<std::string>{"0"} : outsets;
        image.repeat = repeats.empty() ? std::vector<std::string>{"stretch"} : repeats;
        return image.slice.size() <= 4 && image.width.size() <= 4 && image.outset.size() <= 4 && image.repeat.size() <= 2;
    }

    std::string borderImageDecorator(const BorderImage& image){
        const std::vector<std::string> slice = boxValues(image.slice);
        const std::vector<std::string> width = boxValues(image.width);
        const std::vector<std::string> outset = boxValues(image.outset);
        const std::string repeatX = image.repeat[0];
        const std::string repeatY = image.repeat.size() > 1 ? image.repeat[1] : image.repeat[0];
        return "html-border-image(" + serializeString(image.source) + " " + join(slice) + " " + (image.fill ? "fill" : "nofill") + " " +
               join(width) + " " + join(outset) + " " + repeatX + " " + repeatY + ")";
    }

    bool isBorderImageProperty(const std::string& name){
        return name.compare(0, 12, "border-image") == 0 || name.compare(0, 20, "-webkit-border-image") == 0;
    }

}

void jasperengine::htmlui::compat::rewriteBorder(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    const std::vector<int> sides = sidesOf(context.session, n);
    const std::string part = partOf(n);

    if (n.compare(0, 13, "border-inline") == 0 || n.compare(0, 12, "border-block") == 0){
        // logical: the physical properties of each side
        if (context.session.rootDirection == "rtl")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + n + "' is mapped right-to-left (the document root is rtl)");
        for (int side : sides){
            const std::string physical = std::string("border-") + kSides[side] + (part == "all" ? std::string() : "-" + part);
            processSynthetic(context, physical, context.valueText());
        }
        return;
    }
    if (w.size() == 1 && isVar(w[0]) && part != "all"){
        // a var() width or color goes to every side as written
        for (int side : sides)
            context.emit(std::string("border-") + kSides[side] + "-" + part, text(w[0]));
        return;
    }
    if (part == "width"){
        std::vector<std::string> widths;
        for (const ComponentValue& v : w){
            std::optional<std::string> width = widthOf(context.session, v);
            if (!width){
                unsupportedValue(context, "use a length or thin, medium, thick");
                return;
            }
            widths.push_back(*width);
        }
        if (sides.size() == 1){
            if (widths.size() != 1){
                unsupportedValue(context);
                return;
            }
            context.emit(n, widths[0]);
            return;
        }
        std::vector<std::string> box = boxValues(widths);
        if (box.empty() || widths.size() > 4){
            unsupportedValue(context);
            return;
        }
        context.emit("border-width", join(widths));
        return;
    }
    if (part == "color"){
        std::vector<std::string> colors;
        for (const ComponentValue& v : w){
            if (!(isColor(context.session, v) || isVar(v))){
                unsupportedValue(context, "write a CSS color");
                return;
            }
            colors.push_back(text(v));
        }
        if (sides.size() == 1){
            if (colors.size() != 1){
                unsupportedValue(context);
                return;
            }
            context.emit(n, colors[0]);
            return;
        }
        if (colors.empty() || colors.size() > 4){
            unsupportedValue(context);
            return;
        }
        std::vector<std::string> box = boxValues(colors);
        for (int i = 0; i < 4; i++)
            context.emit(std::string("border-") + kSides[i] + "-color", box[(size_t)i]);
        return;
    }

    // border, border-<side>: [width] [style] [color]
    std::string width, style, color;
    bool widthGiven = false;
    if (w.size() == 1 && isVar(w[0])){
        std::vector<ComponentValue> resolved;
        if (!context.session.staticVar(w[0], resolved)){
            context.reportAs(Severity::Warning, codes::VarUntyped, "'" + context.sourceText() +
                             "': a border shorthand from a variable cannot be split into width and color; written as is",
                             "write the width, style and color, with var() for each part");
            context.emit(n, text(w[0]));
            return;
        }
        w = words(resolved);
    }
    for (const ComponentValue& v : w){
        const std::string k = identOf(v);
        if (isStyle(k) && style.empty()){
            style = k;
        }else if (std::optional<std::string> l = widthOf(context.session, v); l && !widthGiven){
            width = *l;
            widthGiven = true;
        }else if ((isColor(context.session, v) || isVar(v)) && color.empty()){
            color = text(v);
        }else{
            unsupportedValue(context, "write border: <width> <style> <color>");
            return;
        }
    }
    if (width.empty())
        width = "3px";
    if (style.empty())
        style = "none";
    if (color.empty()){
        const std::string ruleColor = context.rule ? ruleStaticColor(context.session, *context.rule) : std::string();
        color = ruleColor.empty() ? serialize(ValueNormalizer::currentColorCompanion()) : ruleColor;
    }
    reportStyle(context, style);
    emitStyle(context, sides, style, color, width, widthGiven);
}

void jasperengine::htmlui::compat::rewriteBorderStyle(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    std::vector<std::string> styles;
    for (const ComponentValue& v : w){
        const std::string k = identOf(v);
        if (!isStyle(k)){
            unsupportedValue(context, "use none, solid, dashed, dotted, double, groove, ridge, inset or outset");
            return;
        }
        styles.push_back(k);
    }
    std::vector<int> sides = sidesOf(context.session, n);
    std::vector<std::string> perSide(4);
    if (sides.size() == 1){
        if (styles.size() != 1){
            unsupportedValue(context);
            return;
        }
        perSide[(size_t)sides[0]] = styles[0];
    }else{
        if (styles.empty() || styles.size() > 4){
            unsupportedValue(context);
            return;
        }
        perSide = boxValues(styles);
    }
    if (context.fork(ForkFeature::BorderStyle)){
        for (int side : sides)
            context.emit(std::string("border-") + kSides[side] + "-style", perSide[(size_t)side]);
        return;
    }
    bool reportedMedium = false;
    for (int side : sides){
        const std::string& style = perSide[(size_t)side];
        reportStyle(context, style);
        const std::string property = std::string("border-") + kSides[side] + "-width";
        if (style == "none" || style == "hidden"){
            context.emit(property, "0px");
            continue;
        }
        if (context.rule && ruleSetsWidth(*context.rule, side))
            continue;
        // the initial medium width, or the base rule's
        std::string width = "3px";
        if (context.rule){
            if (const FlatRule* base = BaseRules::find(context.session, *context.rule, [&](const FlatRule& candidate){
                    return ruleSetsWidth(candidate, side);
                })){
                if (std::optional<std::vector<ComponentValue>> value = ruleValue(context.session, *base, property)){
                    if (value->size() == 1 && widthOf(context.session, (*value)[0]))
                        width = *widthOf(context.session, (*value)[0]);
                }
                context.reportAs(Severity::Info, codes::MergedFromBaseRule, "'" + context.sourceText() + "' takes the border width of the base rule at " +
                                 BaseRules::location(context.session, *base));
                context.emit(property, width);
                continue;
            }
        }
        if (!reportedMedium){
            reportedMedium = true;
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + context.sourceText() +
                             "' without a border width in the rule draws the initial medium width (3px)", "set border-width");
        }
        context.emit(property, width);
    }
}

void jasperengine::htmlui::compat::rewriteBorderRadius(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> horizontal;
    bool elliptical = false;
    for (const ComponentValue& v : context.value){
        if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
            elliptical = true;
            break;
        }
        horizontal.push_back(v);
    }
    std::vector<ComponentValue> w = words(horizontal);
    if (elliptical){
        // a / b: equal radii are circular arcs, others are drawn with the horizontal radius
        std::vector<ComponentValue> vertical;
        bool after = false;
        for (const ComponentValue& v : context.value){
            if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                after = true;
                continue;
            }
            if (after)
                vertical.push_back(v);
        }
        if (text(horizontal) != text(vertical))
            context.reportAs(Severity::Warning, codes::ApproximatedVisible, "elliptical radii '" + context.valueText() +
                             "' are drawn with the horizontal radius", "use one radius per corner");
    }
    // the corners: top-left, top-right, bottom-right, bottom-left
    static const char* const corners[] = {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
                                          "border-bottom-left-radius"};
    std::vector<int> targets;
    const bool rtl = context.session.rootDirection == "rtl";
    if (n == "border-radius"){
        targets = {0, 1, 2, 3};
    }else if (n == "border-start-start-radius"){
        targets = {rtl ? 1 : 0};
    }else if (n == "border-start-end-radius"){
        targets = {rtl ? 0 : 1};
    }else if (n == "border-end-end-radius"){
        targets = {rtl ? 3 : 2};
    }else if (n == "border-end-start-radius"){
        targets = {rtl ? 2 : 3};
    }else{
        for (int i = 0; i < 4; i++){
            if (n == corners[i])
                targets = {i};
        }
    }
    if (targets.empty() || w.empty() || (targets.size() == 1 && w.size() != 1) || w.size() > 4){
        unsupportedValue(context, "use one to four lengths");
        return;
    }
    std::vector<std::string> values;
    bool anyPercent = false, anyVar = false;
    for (const ComponentValue& v : w){
        if (isVar(v)){
            values.push_back(text(v));
            anyVar = true;
            continue;
        }
        if (v.isToken(TokenType::Percentage)){
            anyPercent = true;
            values.push_back(text(v));
            continue;
        }
        if (!isLength(context.session, v)){
            unsupportedValue(context, "use lengths");
            return;
        }
        values.push_back(lengthText(v));
    }
    if (anyVar && !anyPercent && targets.size() == 4){
        context.emit("border-radius", join(values));
        return;
    }
    if (!anyPercent || context.fork(ForkFeature::PercentRadius)){
        if (targets.size() == 4)
            context.emit("border-radius", join(values));
        else
            context.emit(corners[targets[0]], values[0]);
        return;
    }
    // percentages: >= 50% is a circle / pill (9999px), smaller ones cannot be drawn
    std::vector<std::string> box = targets.size() == 4 ? boxValues(values) : std::vector<std::string>{values[0]};
    bool reportedPill = false, reportedDropped = false;
    for (size_t i = 0; i < targets.size(); i++){
        std::string value = box[i];
        if (!value.empty() && value.back() == '%'){
            const double percent = std::strtod(value.c_str(), nullptr);
            if (percent >= 50.0){
                if (!reportedPill){
                    reportedPill = true;
                    context.reportAs(Severity::Info, codes::ForkFallback, "radius " + value +
                                     " is drawn as 9999px: a circle on a square box, a stadium (not an ellipse) on a rectangle, until the fork has PercentRadius");
                }
                value = "9999px";
            }else{
                if (!reportedDropped){
                    reportedDropped = true;
                    context.reportAs(Severity::Warning, codes::ForkFallback, "radius " + value + " is dropped until the fork has PercentRadius",
                                     "use a length (px) for this radius");
                }
                continue;
            }
        }
        context.emit(corners[targets[i]], value);
    }
}

void jasperengine::htmlui::compat::rewriteBorderImage(DeclContext& context){
    // every border-image declaration of the rule merges into the last one
    if (context.rule){
        const Declaration* last = nullptr;
        for (const Declaration& d : context.rule->declarations){
            if (isBorderImageProperty(d.name))
                last = &d;
        }
        if (last && last != &context.declaration && last->ref != context.declaration.ref)
            return;
    }
    BorderImage image;
    bool fromRule = false;
    bool sourceSet = false;
    auto apply = [&](const std::string& property, const std::vector<ComponentValue>& value, SourceRef){
        std::string p = property;
        if (p.compare(0, 8, "-webkit-") == 0)
            p = p.substr(8);
        if (p == "border-image" || p == "border-image-source")
            sourceSet = true;
        return applyBorderImage(context, p, value, image);
    };
    if (context.rule){
        bool hasSource = false;
        for (const Declaration& d : context.rule->declarations){
            if (d.name == "border-image" || d.name == "-webkit-border-image" || d.name == "border-image-source")
                hasSource = true;
        }
        if (!hasSource){
            const FlatRule* base = BaseRules::find(context.session, *context.rule, [](const FlatRule& candidate){
                for (const Declaration& d : candidate.declarations){
                    if (d.name == "border-image" || d.name == "-webkit-border-image" || d.name == "border-image-source")
                        return true;
                }
                return false;
            });
            if (base){
                for (const Declaration& d : base->declarations){
                    if (isBorderImageProperty(d.name))
                        apply(d.name, d.value, d.ref);
                }
                context.reportAs(Severity::Info, codes::MergedFromBaseRule, "border-image longhands are merged with the base rule at " +
                                 BaseRules::location(context.session, *base));
            }else{
                context.reportAs(Severity::Warning, codes::LonghandWithoutBase, "'" + context.name +
                                 "' without a border-image source in the rule or a base rule is dropped", "set border-image in the same rule");
                context.dropped = true;
                return;
            }
        }
        for (const Declaration& d : context.rule->declarations){
            if (!isBorderImageProperty(d.name))
                continue;
            fromRule = true;
            NormalizeContext normalize;
            normalize.property = d.name;
            normalize.options = context.session.options;
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, normalize);
            if (!normalized.ok || !apply(d.name, trimWhitespace(normalized.value), d.ref)){
                context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(d.value) + "' is not supported; dropped");
            }
        }
    }
    if (!fromRule && !apply(context.name, context.value, context.declaration.ref)){
        unsupportedValue(context, "write border-image: url(...) <slice> / <width> <repeat>");
        return;
    }
    (void)sourceSet;
    if (image.gradient){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "border-image with a gradient source is not supported; dropped",
                         "use an image file as the source");
        context.dropped = true;
        return;
    }
    companions::emitBorderImage(context, image.source.empty() ? std::string() : borderImageDecorator(image));
}

namespace {

    // `border-collapse: collapse` in the declaration's rule: CSS ignores border-spacing then
    bool collapsesBorders(const DeclContext& context){
        if (!context.rule)
            return false;
        for (const Declaration& d : context.rule->declarations){
            if (d.name != "border-collapse")
                continue;
            const std::vector<ComponentValue> value = words(d.value);
            if (value.size() == 1 && identOf(value[0]) == "collapse")
                return true;
        }
        return false;
    }

}

void jasperengine::htmlui::compat::rewriteBorderSpacing(DeclContext& context){
    if (context.name == "border-collapse"){
        std::vector<ComponentValue> w = words(context.value);
        const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
        if (k == "separate")
            return;   // the engine's (only) model
        if (k != "collapse"){
            unsupportedValue(context, "use separate or collapse");
            return;
        }
        // the collapsing model's spacing is none; its merged borders are not drawn
        context.reportAs(Severity::Warning, codes::ApproximatedVisible,
                         "'border-collapse: collapse' keeps every cell's borders: adjacent borders are drawn twice",
                         "set borders on one side of each cell (border-bottom on cells, for example)");
        context.emit("gap", "0px");
        return;
    }
    if (collapsesBorders(context))
        return;   // the rule collapses borders: no spacing, as in CSS
    std::vector<ComponentValue> w = words(context.value);
    if (w.empty() || w.size() > 2){
        unsupportedValue(context);
        return;
    }
    for (const ComponentValue& v : w){
        if (!isLength(context.session, v) && !isVar(v)){
            unsupportedValue(context, "use one or two lengths");
            return;
        }
    }
    const std::string x = lengthText(w[0]);
    const std::string y = w.size() > 1 ? lengthText(w[1]) : x;
    context.emit("gap", y == x ? x : y + " " + x);
}
