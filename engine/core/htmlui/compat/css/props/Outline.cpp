// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Outline and box-shadow (phase-2 §3.9.7 "Outline", "Box shadow"): RmlUi has no outline, so it
// is drawn as the first entry of the box-shadow list. Both go through companions
// (--html-outline-shadow, --html-box-shadow) so an outline never overwrites the element's own
// shadows and vice versa.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/Companions.h"
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

    // the UA focus ring (Chromium's two-tone auto outline)
    constexpr const char* kAutoRing = "0 0 0 1px #ffffffff, 0 0 0 3px #101010ff";

    bool isOutlineStyle(const std::string& k){
        return k == "none" || k == "auto" || k == "solid" || k == "dotted" || k == "dashed" || k == "double" || k == "groove" ||
               k == "ridge" || k == "inset" || k == "outset" || k == "hidden";
    }

    struct Outline {
        std::optional<ComponentValue> width;   // default medium
        std::string style = "none";
        std::string color;                     // "" = currentColor
        std::optional<ComponentValue> offset;
        bool invalid = false;
    };

    std::optional<ComponentValue> widthToken(Session& session, const ComponentValue& v){
        const std::string k = identOf(v);
        if (k == "thin")
            return ValueNormalizer::dimension(1, "px");
        if (k == "medium")
            return ValueNormalizer::dimension(3, "px");
        if (k == "thick")
            return ValueNormalizer::dimension(5, "px");
        if (isLength(session, v))
            return v;
        return std::nullopt;
    }

    void applyOutline(Session& session, const std::string& property, const std::vector<ComponentValue>& value, Outline& outline){
        std::vector<ComponentValue> w = words(value);
        if (property == "outline"){
            outline = Outline();
            for (const ComponentValue& v : w){
                const std::string k = identOf(v);
                if (isOutlineStyle(k)){
                    outline.style = k;
                }else if (std::optional<ComponentValue> width = widthToken(session, v)){
                    outline.width = width;
                }else if (k == "invert"){
                    outline.color = "#000000ff";
                }else if (isColor(session, v) || isVar(v)){
                    outline.color = text(v);
                }else{
                    outline.invalid = true;
                }
            }
            return;
        }
        if (w.size() != 1){
            outline.invalid = true;
            return;
        }
        const ComponentValue& v = w[0];
        if (property == "outline-width"){
            if (std::optional<ComponentValue> width = widthToken(session, v))
                outline.width = width;
            else
                outline.invalid = true;
        }else if (property == "outline-style"){
            if (isOutlineStyle(identOf(v)))
                outline.style = identOf(v);
            else
                outline.invalid = true;
        }else if (property == "outline-color"){
            if (isIdent(v, "invert"))
                outline.color = "#000000ff";
            else if (isIdent(v, "auto"))
                outline.color.clear();
            else if (isColor(session, v) || isVar(v))
                outline.color = text(v);
            else
                outline.invalid = true;
        }else if (property == "outline-offset"){
            if (isLength(session, v))
                outline.offset = v;
            else
                outline.invalid = true;
        }
    }

    // width + offset when they add up statically
    std::optional<std::string> spread(const ComponentValue& width, const std::optional<ComponentValue>& offset){
        if (!offset)
            return lengthText(width);
        const bool widthZero = width.isToken(TokenType::Number) || width.token.number == 0;
        const bool offsetZero = offset->isToken(TokenType::Number) || offset->token.number == 0;
        if (offsetZero)
            return lengthText(width);
        if (widthZero)
            return lengthText(*offset);
        if (width.isToken(TokenType::Dimension) && offset->isToken(TokenType::Dimension) && equalsNoCase(width.token.unit, offset->token.unit))
            return formatNumber(width.token.number + offset->token.number) + lowerAscii(width.token.unit);
        std::optional<double> a = lengthPx(width), b = lengthPx(*offset);
        if (a && b)
            return formatNumber(*a + *b) + "px";
        return std::nullopt;
    }

    bool isOutlineProperty(const std::string& name){
        return name == "outline" || name == "outline-width" || name == "outline-style" || name == "outline-color" || name == "outline-offset";
    }

}

void jasperengine::htmlui::compat::rewriteOutline(DeclContext& context){
    // every outline declaration of the rule merges into the last one
    Outline outline;
    if (context.rule){
        const Declaration* last = nullptr;
        for (const Declaration& d : context.rule->declarations){
            if (isOutlineProperty(d.name))
                last = &d;
        }
        if (last && last->ref != context.declaration.ref)
            return;
        for (const Declaration& d : context.rule->declarations){
            if (!isOutlineProperty(d.name))
                continue;
            if (d.ref == context.declaration.ref){
                applyOutline(context.session, d.name, context.value, outline);
                continue;
            }
            NormalizeContext normalize;
            normalize.property = d.name;
            normalize.options = context.session.options;
            normalize.staticColor = ruleStaticColor(context.session, *context.rule);
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, normalize);
            if (normalized.ok)
                applyOutline(context.session, d.name, trimWhitespace(normalized.value), outline);
        }
        // a rule that sets only outline-offset / -color / -width keeps the style none of CSS's
        // initial value: nothing is drawn, as in CSS
    }else{
        applyOutline(context.session, context.name, context.value, outline);
    }
    if (outline.invalid){
        unsupportedValue(context, "write outline: <width> <style> <color>");
        return;
    }
    const ComponentValue width = outline.width ? *outline.width : ValueNormalizer::dimension(3, "px");
    std::string ring;
    if (outline.style == "none" || outline.style == "hidden" || (width.token.number == 0)){
        ring = companions::kNoShadow;
    }else if (outline.style == "auto"){
        ring = kAutoRing;
    }else{
        if (outline.style != "solid")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'outline-style: " + outline.style + "' is drawn solid");
        std::string color = outline.color;
        if (color.empty()){
            const std::string ruleColor = context.rule ? ruleStaticColor(context.session, *context.rule) : std::string();
            color = ruleColor.empty() ? serialize(ValueNormalizer::currentColorCompanion()) : ruleColor;
        }
        const bool negative = outline.offset && outline.offset->token.number < 0;
        if (negative){
            ring = "inset 0 0 0 " + lengthText(width) + " " + color;
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "a negative outline-offset is drawn as an inset ring of the outline width");
        }else{
            std::optional<std::string> s = spread(width, outline.offset);
            if (!s){
                s = lengthText(width);
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "outline width and offset in different units: the offset is dropped");
            }else if (outline.offset && outline.offset->token.number > 0){
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "the gap of outline-offset is filled with the outline color");
            }
            ring = "0 0 0 " + *s + " " + color;
        }
    }
    companions::emitOutline(context, ring);
}

void jasperengine::htmlui::compat::rewriteBoxShadow(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "none")){
        companions::emitBoxShadow(context, "none");
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        companions::emitBoxShadow(context, text(w[0]));
        return;
    }
    // each shadow: 2-4 lengths, optional color (currentColor when missing), optional inset
    std::vector<std::string> shadows;
    for (const std::vector<ComponentValue>& group : commaGroups(context.value)){
        int lengths = 0;
        bool color = false;
        bool hasVar = false;
        std::vector<std::string> parts;
        for (const ComponentValue& v : words(group)){
            if (isIdent(v, "inset")){
                parts.push_back("inset");
            }else if (isLength(context.session, v)){
                lengths++;
                parts.push_back(lengthText(v));
            }else if (isColor(context.session, v)){
                color = true;
                parts.push_back(text(v));
            }else if (isVar(v)){
                hasVar = true;
                parts.push_back(text(v));
            }else{
                unsupportedValue(context, "write box-shadow: [inset] <x> <y> [<blur> [<spread>]] [<color>]");
                return;
            }
        }
        if (!hasVar && (lengths < 2 || lengths > 4)){
            unsupportedValue(context, "write box-shadow: [inset] <x> <y> [<blur> [<spread>]] [<color>]");
            return;
        }
        if (!color && !hasVar){
            const std::string ruleColor = context.rule ? ruleStaticColor(context.session, *context.rule) : std::string();
            parts.push_back(ruleColor.empty() ? serialize(ValueNormalizer::currentColorCompanion()) : ruleColor);
        }
        shadows.push_back(join(parts));
    }
    companions::emitBoxShadow(context, join(shadows, ", "));
}
