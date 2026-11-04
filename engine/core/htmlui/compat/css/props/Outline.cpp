// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Outline and box-shadow (phase-2 §3.9.7 "Outline", "Box shadow"): RmlUi has no outline, so it
// is drawn as the first entry of the box-shadow list. Both go through companions
// (--html-outline-shadow, --html-box-shadow) so an outline never overwrites the element's own
// shadows and vice versa. The outline declarations of a rule are read into one structure (the
// values as written, keywords as views), the ring and the shadow list are written once into one
// buffer.

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

    bool isOutlineStyle(std::string_view k){
        return k == "none" || k == "auto" || k == "solid" || k == "dotted" || k == "dashed" || k == "double" || k == "groove" ||
               k == "ridge" || k == "inset" || k == "outset" || k == "hidden";
    }

    // a length in px (the widths of thin, medium and thick)
    ComponentValue pixels(double value){
        ComponentValue v;
        v.token.type = TokenType::Dimension;
        v.token.number = value;
        v.token.unit = Text::view("px");
        return v;
    }

    const ComponentValue& thin(){ static const ComponentValue v = pixels(1); return v; }
    const ComponentValue& medium(){ static const ComponentValue v = pixels(3); return v; }
    const ComponentValue& thick(){ static const ComponentValue v = pixels(5); return v; }

    struct Outline {
        const ComponentValue* width = nullptr;     // null: medium
        std::string_view style = "none";
        // the color: currentColor (null, not invert), invert (#000000ff) or the value written
        const ComponentValue* color = nullptr;
        bool invert = false;
        const ComponentValue* offset = nullptr;
        bool invalid = false;
    };

    const ComponentValue* widthOf(Session& session, const ComponentValue& v){
        const std::string_view k = identView(v);
        if (k == "thin")
            return &thin();
        if (k == "medium")
            return &medium();
        if (k == "thick")
            return &thick();
        if (isLength(session, v))
            return &v;
        return nullptr;
    }

    void applyOutline(Session& session, std::string_view property, const ValueList& value, Outline& outline){
        const Words w = wordsOf(value);
        if (property == "outline"){
            outline = Outline();
            for (const ComponentValue* v : w){
                const std::string_view k = identView(*v);
                if (isOutlineStyle(k)){
                    outline.style = k;
                }else if (const ComponentValue* width = widthOf(session, *v)){
                    outline.width = width;
                }else if (k == "invert"){
                    outline.color = nullptr;
                    outline.invert = true;
                }else if (isColor(session, *v) || isVar(*v)){
                    outline.color = v;
                    outline.invert = false;
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
        const ComponentValue& v = *w[0];
        if (property == "outline-width"){
            if (const ComponentValue* width = widthOf(session, v))
                outline.width = width;
            else
                outline.invalid = true;
        }else if (property == "outline-style"){
            if (isOutlineStyle(identView(v)))
                outline.style = identView(v);
            else
                outline.invalid = true;
        }else if (property == "outline-color"){
            if (isIdent(v, "invert")){
                outline.color = nullptr;
                outline.invert = true;
            }else if (isIdent(v, "auto")){
                outline.color = nullptr;
                outline.invert = false;
            }else if (isColor(session, v) || isVar(v)){
                outline.color = &v;
                outline.invert = false;
            }else{
                outline.invalid = true;
            }
        }else if (property == "outline-offset"){
            if (isLength(session, v))
                outline.offset = &v;
            else
                outline.invalid = true;
        }
    }

    // lengthText() appended
    void appendLength(std::string& out, const ComponentValue& value){
        if (value.isToken(TokenType::Number) && value.token.number == 0)
            out += "0px";
        else
            appendSerialized(out, value);
    }

    // width + offset when they add up statically, appended to `out`; false when they do not
    bool appendSpread(std::string& out, const ComponentValue& width, const ComponentValue* offset){
        if (!offset){
            appendLength(out, width);
            return true;
        }
        const bool widthZero = width.isToken(TokenType::Number) || width.token.number == 0;
        const bool offsetZero = offset->isToken(TokenType::Number) || offset->token.number == 0;
        if (offsetZero){
            appendLength(out, width);
            return true;
        }
        if (widthZero){
            appendLength(out, *offset);
            return true;
        }
        if (width.isToken(TokenType::Dimension) && offset->isToken(TokenType::Dimension) && equalsNoCase(width.token.unit, offset->token.unit)){
            appendNumber(out, width.token.number + offset->token.number);
            out += lowerAscii(width.token.unit);
            return true;
        }
        std::optional<double> a = lengthPx(width), b = lengthPx(*offset);
        if (a && b){
            appendNumber(out, *a + *b);
            out += "px";
            return true;
        }
        return false;
    }

    bool isOutlineProperty(std::string_view name){
        return name == "outline" || name == "outline-width" || name == "outline-style" || name == "outline-color" || name == "outline-offset";
    }

    // the color of a ring or shadow without one: the rule's static color, else the current-color companion
    void appendCurrentColor(std::string& out, DeclContext& context){
        const std::string& ruleColor = context.rule ? ruleStaticColor(context.session, *context.rule) : std::string();
        if (ruleColor.empty())
            appendSerialized(out, ValueNormalizer::currentColorCompanion());
        else
            out += ruleColor;
    }

    // (one buffer for the rings and shadow lists of the thread: the emitted value is a copy)
    std::string& shadowBuffer(){
        static thread_local std::string buffer;
        return buffer;
    }

}

void jasperengine::htmlui::compat::rewriteOutline(DeclContext& context){
    if (context.fork(ForkFeature::Outline)){
        // the fork draws outlines (phase 3 §3.7.4): every declaration passes through; CSS UI 4's
        // 'outline-color: auto' is the current color there, 'hidden' is not an outline style
        const Words w = wordsOf(context.value);
        if (w.empty()){
            unsupportedValue(context, "write outline: <width> <style> <color>");
            return;
        }
        std::string out;
        for (const ComponentValue* v : w){
            if (isIdent(*v, "hidden")){
                unsupportedValue(context, "use outline-style: none");
                return;
            }
            if (!out.empty())
                out += ' ';
            if (context.name == "outline-color" && isIdent(*v, "auto"))
                out += "currentcolor";
            else if (context.name == "outline-offset" || ((context.name == "outline" || context.name == "outline-width") && isLength(context.session, *v)))
                appendLength(out, *v);
            else
                appendSerialized(out, *v);
        }
        context.emit(context.name, out);
        return;
    }
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
            normalize.options = &context.session.options;
            normalize.staticColor = ruleStaticColor(context.session, *context.rule);
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, normalize);
            if (normalized.ok){
                // (the normalized values live in the session's arena, as the outline's views of them)
                const ValueList trimmed = trimWhitespace(normalized.value);
                applyOutline(context.session, d.name, trimmed, outline);
            }
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
    const ComponentValue& width = outline.width ? *outline.width : medium();
    std::string& ring = shadowBuffer();
    ring.clear();
    if (outline.style == "none" || outline.style == "hidden" || (width.token.number == 0)){
        ring = companions::kNoShadow;
    }else if (outline.style == "auto"){
        ring = kAutoRing;
    }else{
        if (outline.style != "solid")
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'outline-style: " + std::string(outline.style) + "' is drawn solid");
        // the color after the ring, written into the same buffer first
        std::string color;
        if (outline.invert)
            color = "#000000ff";
        else if (outline.color)
            appendSerialized(color, *outline.color);
        else
            appendCurrentColor(color, context);
        const bool negative = outline.offset && outline.offset->token.number < 0;
        if (negative){
            ring += "inset 0 0 0 ";
            appendLength(ring, width);
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "a negative outline-offset is drawn as an inset ring of the outline width");
        }else{
            ring += "0 0 0 ";
            if (!appendSpread(ring, width, outline.offset)){
                ring.resize(6);
                appendLength(ring, width);
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "outline width and offset in different units: the offset is dropped");
            }else if (outline.offset && outline.offset->token.number > 0){
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "the gap of outline-offset is filled with the outline color");
            }
        }
        ring += ' ';
        ring += color;
    }
    companions::emitOutline(context, ring);
}

void jasperengine::htmlui::compat::rewriteBoxShadow(DeclContext& context){
    const Words w = wordsOf(context.value);
    if (w.size() == 1 && isIdent(*w[0], "none")){
        companions::emitBoxShadow(context, "none");
        return;
    }
    if (w.size() == 1 && isVar(*w[0])){
        companions::emitBoxShadow(context, text(*w[0]));
        return;
    }
    // each shadow: 2-4 lengths, optional color (currentColor when missing), optional inset
    std::string& shadows = shadowBuffer();
    shadows.clear();
    for (const ValueRange& group : commaRanges(context.value)){
        int lengths = 0;
        bool color = false;
        bool hasVar = false;
        if (!shadows.empty())
            shadows += ", ";
        const size_t shadowStart = shadows.size();
        auto separate = [&](){
            if (shadows.size() > shadowStart)
                shadows += ' ';
        };
        for (const ComponentValue* v : wordsOf(group)){
            if (isIdent(*v, "inset")){
                separate();
                shadows += "inset";
            }else if (isLength(context.session, *v)){
                lengths++;
                separate();
                appendLength(shadows, *v);
            }else if (isColor(context.session, *v)){
                color = true;
                separate();
                appendSerialized(shadows, *v);
            }else if (isVar(*v)){
                hasVar = true;
                separate();
                appendSerialized(shadows, *v);
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
            separate();
            // the fork resolves currentcolor in a shadow where it is drawn (ForkFeature::CurrentColor): the
            // element's color, also while it changes; without it the rule's static color or the companion
            if (context.session.fork(ForkFeature::CurrentColor))
                shadows += "currentcolor";
            else
                appendCurrentColor(shadows, context);
        }
    }
    companions::emitBoxShadow(context, shadows);
}
