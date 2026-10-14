// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Backgrounds and masks (phase-2 §3.9.7 "Background", Effects "mask-image"): CSS background
// layers become RmlUi decorators (first layer on top, as in CSS): gradients as RmlUi's native
// gradient decorators with normalized arguments, url() / image-set() images as the compat
// html-layer decorator with all ten arguments (size, position, repeat, origin, clip,
// attachment); background-color stays native. Longhands merge with the rule's shorthand or a
// base rule; gradient text falls back to a solid color.
//
// Each declaration of the family is read once into Layers (in the session's arena: keywords as
// enums, lengths and colors as the component values they are written as), the family's
// declarations are merged on that structure, and the decorator list is written once, into
// one buffer, when the family's last declaration is rewritten.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"
#include "htmlui/compat/CompatAtProperty.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"
#include "htmlui/effects/EffectCompat.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    // where layer diagnostics go and how URLs resolve: to the declaration being rewritten
    // (`context`), to the session's sink at `ref` (`toSink`), or nowhere
    struct LayerScope {
        Session& session;
        uint32_t file = 0;
        SourceRef ref;
        bool mask = false;
        DeclContext* context = nullptr;
        bool toSink = false;
    };

    void say(LayerScope& scope, Severity severity, DiagCode code, const std::string& message, const std::string& suggestion = std::string()){
        if (scope.context)
            scope.context->reportAs(severity, code, message, suggestion);
        else if (scope.toSink)
            scope.session.sink->reportAs(severity, code, scope.ref, message, suggestion);
    }

    // a literal ident component value (the centered axis of background-position-x/y)
    ComponentValue literalIdent(const char* name){
        ComponentValue value;
        value.token.type = TokenType::Ident;
        value.token.value = Text::view(name);
        return value;
    }

    bool startsWith(std::string_view text, std::string_view prefix){
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    // ---- positions

    // one axis of a position: <percentage> + <length> offset (edge offsets: right 10px = 100% - 10px)
    struct Axis {
        enum class Length : uint8_t { None, Px, Value };
        double percent = 0;
        // the length offset: none, `px` (written formatNumber(px) + "px"), or the text of
        // `value`; `negated` flips the sign of what is written
        Length length = Length::None;
        bool negated = false;
        double px = 0;
        const ComponentValue* value = nullptr;
        const ComponentValue* raw = nullptr;   // a length-percentage written as it is (var(), em, ...)
    };

    void appendLength(std::string& out, const Axis& axis){
        const size_t at = out.size();
        if (axis.length == Axis::Length::Px){
            appendNumber(out, axis.px);
            out += "px";
        }else{
            appendSerialized(out, *axis.value);
        }
        if (axis.negated){
            if (out.size() > at && out[at] == '-')
                out.erase(at, 1);
            else
                out.insert(at, 1, '-');
        }
    }

    void appendAxis(std::string& out, const Axis& axis){
        if (axis.raw){
            appendSerialized(out, *axis.raw);
            return;
        }
        if (axis.length == Axis::Length::None){
            appendNumber(out, axis.percent);
            out += '%';
            return;
        }
        if (axis.percent == 0){
            appendLength(out, axis);
            return;
        }
        // html-layer reads calc(<percentage> +|- <length>) (HtmlLayerDecorator, 3.11.3)
        out += "calc(";
        appendNumber(out, axis.percent);
        out += "% ";
        const size_t at = out.size();
        appendLength(out, axis);
        if (out[at] == '-')
            out.replace(at, 1, "- ");
        else
            out.insert(at, "+ ");
        out += ')';
    }

    // the RmlUi gradient position (no combined offsets)
    void appendGradientAxis(std::string& out, const Axis& axis, bool& lost){
        if (axis.raw){
            appendSerialized(out, *axis.raw);
            return;
        }
        if (axis.length != Axis::Length::None && axis.percent != 0){
            lost = true;
            appendNumber(out, axis.percent);
            out += '%';
            return;
        }
        if (axis.length != Axis::Length::None){
            appendLength(out, axis);
            return;
        }
        appendNumber(out, axis.percent);
        out += '%';
    }

    // is the axis written "0%"?
    bool atZero(const Axis& axis){
        if (axis.raw || axis.length != Axis::Length::None)
            return false;
        std::string written;
        appendNumber(written, axis.percent);
        return written == "0";
    }

    // CSS <position> (1-4 values) into x and y
    bool parsePosition(Session& session, const Words& w, Axis& x, Axis& y){
        if (w.empty() || w.size() > 4)
            return false;
        auto isLP = [&](const ComponentValue& v){ return isLength(session, v, true) || isVar(v); };
        auto setLP = [&](Axis& axis, const ComponentValue& v){
            if (v.isToken(TokenType::Percentage)){
                axis.percent = v.token.number;
            }else if (std::optional<double> px = lengthPx(v)){
                axis.percent = 0;
                axis.length = Axis::Length::Px;
                axis.px = *px;
            }else{
                axis.raw = &v;
            }
        };
        auto edge = [](std::string_view k, bool& horizontal, double& percent) -> bool {
            if (k == "left"){ horizontal = true; percent = 0; return true; }
            if (k == "right"){ horizontal = true; percent = 100; return true; }
            if (k == "top"){ horizontal = false; percent = 0; return true; }
            if (k == "bottom"){ horizontal = false; percent = 100; return true; }
            return false;
        };
        x = Axis();
        y = Axis();
        x.percent = 50;
        y.percent = 50;
        if (w.size() == 1){
            const std::string_view k = identView(*w[0]);
            bool horizontal = false;
            double percent = 0;
            if (k == "center")
                return true;
            if (edge(k, horizontal, percent)){
                (horizontal ? x : y).percent = percent;
                return true;
            }
            if (isLP(*w[0])){
                setLP(x, *w[0]);
                return true;
            }
            return false;
        }
        if (w.size() == 2){
            const std::string_view a = identView(*w[0]), b = identView(*w[1]);
            bool ha = false, hb = false;
            double pa = 50, pb = 50;
            const bool ea = edge(a, ha, pa), eb = edge(b, hb, pb);
            // "top left" / "left top" / "center top" / "50% top" / "left 20%"
            if ((ea || a == "center") && (eb || b == "center")){
                if (ea && eb && ha == hb)
                    return false;
                if ((ea && !ha) || (eb && hb)){
                    // vertical first
                    y.percent = ea ? pa : 50;
                    x.percent = eb ? pb : 50;
                }else{
                    x.percent = ea ? pa : 50;
                    y.percent = eb ? pb : 50;
                }
                return true;
            }
            if ((ea && ha) || a == "center" || isLP(*w[0])){
                if (ea)
                    x.percent = pa;
                else if (a == "center")
                    x.percent = 50;
                else
                    setLP(x, *w[0]);
                if (eb && !hb)
                    y.percent = pb;
                else if (b == "center")
                    y.percent = 50;
                else if (isLP(*w[1]))
                    setLP(y, *w[1]);
                else
                    return false;
                return true;
            }
            return false;
        }
        // 3-4 values: edge [offset] edge [offset]
        size_t i = 0;
        bool xSet = false, ySet = false;
        while (i < w.size()){
            const std::string_view k = identView(*w[i]);
            bool horizontal = false;
            double percent = 50;
            if (k == "center"){
                i++;
                continue;
            }
            if (!edge(k, horizontal, percent))
                return false;
            Axis& axis = horizontal ? x : y;
            if (horizontal ? xSet : ySet)
                return false;
            (horizontal ? xSet : ySet) = true;
            axis = Axis();
            axis.percent = percent;
            i++;
            if (i < w.size() && isLP(*w[i])){
                const ComponentValue& offset = *w[i];
                if (offset.isToken(TokenType::Percentage)){
                    axis.percent = percent == 100 ? 100 - offset.token.number : offset.token.number;
                }else{
                    if (std::optional<double> px = lengthPx(offset)){
                        axis.length = Axis::Length::Px;
                        axis.px = *px;
                    }else{
                        axis.length = Axis::Length::Value;
                        axis.value = &offset;
                    }
                    axis.negated = percent == 100;
                }
                i++;
            }
        }
        return true;
    }

    // ---- gradients

    bool isGradientName(std::string_view name){
        return name == "linear-gradient" || name == "radial-gradient" || name == "conic-gradient" || name == "repeating-linear-gradient" ||
               name == "repeating-radial-gradient" || name == "repeating-conic-gradient";
    }

    std::string_view legacySideToStandard(std::string_view side){
        if (side == "top") return "bottom";
        if (side == "bottom") return "top";
        if (side == "left") return "right";
        if (side == "right") return "left";
        return side;
    }

    // a function's name in lower case (a view of it when it is lower case already)
    std::string_view lowerName(const ComponentValue& function){
        const Text& name = function.name();
        for (const char c : name){
            if (c >= 'A' && c <= 'Z')
                return Arena::current().copy(lowerAscii(name));
        }
        return name.view();
    }

    // the vendor prefix of a name ("" when none)
    std::string_view vendorPrefix(std::string_view name){
        for (std::string_view prefix : {std::string_view("-webkit-"), std::string_view("-moz-"), std::string_view("-o-"), std::string_view("-ms-")}){
            if (startsWith(name, prefix))
                return prefix;
        }
        return std::string_view();
    }

    bool isAngle(const ComponentValue& v){
        return v.isToken(TokenType::Dimension) && unitCategory(v.token.unit) == UnitCategory::Angle;
    }

    // color stops written to `out`: hints dropped, positions normalized; false on a malformed stop
    bool colorStops(LayerScope& scope, const ArenaList<ValueRange>& groups, size_t first, bool angular, std::string& out,
                    const ComponentValue** middleColor){
        Words colors;
        bool reportedHint = false;
        bool any = false;
        // the fork's layers substitute a var() stop position (a registered <percentage> animates the stop; phase 5)
        const bool native = scope.context && scope.context->fork(ForkFeature::BackgroundLayers);
        for (size_t i = first; i < groups.size(); i++){
            const Words w = wordsOf(groups[i]);
            if (w.empty())
                return false;
            if (w.size() == 1 && (w[0]->isToken(TokenType::Percentage) || isLength(scope.session, *w[0]) || (angular && isAngle(*w[0])))){
                if (!reportedHint){
                    reportedHint = true;
                    say(scope, Severity::Info, codes::ApproximatedMinor, "gradient interpolation hints are dropped (the midpoint stays in the middle)");
                }
                continue;
            }
            // color [pos [pos]] (the color may also come after the position)
            const ComponentValue* color = nullptr;
            const ComponentValue* positions[2] = {nullptr, nullptr};
            size_t positionCount = 0;
            for (const ComponentValue* v : w){
                if (!color && (isColor(scope.session, *v) || isVar(*v))){
                    color = v;
                }else if (v->isToken(TokenType::Percentage) || isLength(scope.session, *v) || (angular && isAngle(*v)) || (native && isVar(*v))){
                    if (positionCount < 2)
                        positions[positionCount] = v;
                    positionCount++;
                }else{
                    return false;
                }
            }
            if (!color || positionCount > 2)
                return false;
            colors.push_back(color);
            if (any)
                out += ", ";
            any = true;
            appendSerialized(out, *color);
            for (size_t p = 0; p < positionCount; p++){
                out += ' ';
                if (positions[p]->isToken(TokenType::Number))
                    out += "0%";
                else
                    appendSerialized(out, *positions[p]);
            }
        }
        if (!any)
            return false;
        if (middleColor && !colors.empty())
            *middleColor = colors[colors.size() / 2];
        return true;
    }

    // a gradient function as RmlUi's decorator, appended to `out`; false when it cannot be
    // written (`out` then holds a part of it)
    bool gradient(LayerScope& scope, const ComponentValue& function, std::string& out, const ComponentValue** middleColor){
        std::string_view name = lowerName(function);
        bool legacy = false;
        const std::string_view prefixed = name;
        for (std::string_view prefix : {std::string_view("-webkit-"), std::string_view("-moz-"), std::string_view("-o-"), std::string_view("-ms-")}){
            if (startsWith(name, prefix)){
                name = name.substr(prefix.size());
                legacy = true;
            }
        }
        if (!isGradientName(name))
            return false;
        if (legacy)
            say(scope, Severity::Info, codes::VendorPrefix, "'" + std::string(prefixed) + "()' is read as '" + std::string(name) + "()'");
        ArenaList<ValueRange> groups = commaRanges(function.children);
        if (groups.empty())
            return false;
        const bool linear = name.find("linear") != std::string_view::npos;
        const bool radial = name.find("radial") != std::string_view::npos;
        const bool conic = name.find("conic") != std::string_view::npos;

        // a color-interpolation method ("in oklch") anywhere in the first group is dropped
        Words head = wordsOf(groups[0]);
        bool interpolation = false;
        for (size_t i = 0; i + 1 < head.size(); i++){
            if (isIdent(*head[i], "in")){
                interpolation = true;
                say(scope, Severity::Info, codes::ApproximatedMinor, "gradient color interpolation '" + text(*head[i + 1]) +
                    "' is dropped (sRGB interpolation)");
                size_t end = i + 2;
                if (end + 1 < head.size() && isIdent(*head[end + 1], "hue"))
                    end += 2;
                head.erase(head.begin() + (std::ptrdiff_t)i, head.begin() + (std::ptrdiff_t)std::min(end, head.size()));
                break;
            }
        }
        bool headIsStop = false;
        if (!head.empty() && (isColor(scope.session, *head[0]) || isVar(*head[0])))
            headIsStop = true;
        // a var() of a registered <angle> is the gradient's angle, which the fork's layers substitute (phase 5)
        const bool angleVar = linear && head.size() == 1 && isVar(*head[0]) && scope.context &&
                              scope.context->fork(ForkFeature::BackgroundLayers) && [&](){
                                  const RegisteredProperty* registered = CompatAtProperty::find(scope.session,
                                                                                                CustomPropertyIndex::varName(*head[0]));
                                  return registered && registered->type == AtPropertySyntax::Angle;
                              }();
        if (angleVar)
            headIsStop = false;
        if (head.empty() && groups[0].size() > 0)
            headIsStop = false;
        out += name;
        out += '(';
        const size_t prefixStart = out.size();
        bool lostOffset = false;
        if (!headIsStop && !head.empty()){
            if (linear){
                if (angleVar){
                    appendSerialized(out, *head[0]);
                }else if (head.size() == 1 && isAngle(*head[0])){
                    double angle = head[0]->token.number;
                    if (legacy)
                        angle = std::fmod(90.0 - angle + 360.0 * 4, 360.0);
                    appendNumber(out, angle);
                    out += "deg";
                }else if (head.size() == 1 && head[0]->isToken(TokenType::Number) && head[0]->token.number == 0){
                    out += legacy ? "90deg" : "0deg";
                }else if (isIdent(*head[0], "to") && !legacy){
                    // "to " and the sides (idents; anything else writes nothing)
                    out += "to ";
                    bool first = true;
                    for (size_t i = 1; i < head.size(); i++){
                        const std::string_view side = identView(*head[i]);
                        if (side.empty())
                            continue;
                        if (!first)
                            out += ' ';
                        first = false;
                        out += side;
                    }
                }else if (legacy){
                    ArenaList<std::string_view> sides;
                    for (const ComponentValue* v : head){
                        const std::string_view k = identView(*v);
                        if (k == "center")
                            continue;
                        if (k != "top" && k != "bottom" && k != "left" && k != "right")
                            return false;
                        sides.push_back(legacySideToStandard(k));
                    }
                    if (!sides.empty()){
                        out += "to ";
                        for (size_t i = 0; i < sides.size(); i++){
                            if (i > 0)
                                out += ' ';
                            out += sides[i];
                        }
                    }
                }else{
                    return false;
                }
            }else if (radial){
                // [shape] [size] [at position] in RmlUi's order; legacy: <position>, <shape> <size>
                std::string_view shape, sizeKeyword[2];
                const ComponentValue* sizeValue[2] = {nullptr, nullptr};
                bool sizeSet[2] = {false, false};
                Words position;
                Words shapeWords = head;
                if (legacy && groups.size() > 2){
                    // -webkit-radial-gradient(center, circle cover, ...)
                    const Words second = wordsOf(groups[1]);
                    const bool secondIsShape = !second.empty() && !isColor(scope.session, *second[0]) && !isVar(*second[0]);
                    position = head;
                    shapeWords = secondIsShape ? second : Words();
                    if (secondIsShape)
                        groups.erase(groups.begin() + 1);
                }
                bool atWord = false;
                for (const ComponentValue* v : shapeWords){
                    const std::string_view k = identView(*v);
                    if (atWord){
                        position.push_back(v);
                        continue;
                    }
                    if (k == "at"){
                        atWord = true;
                        continue;
                    }
                    if (k == "circle" || k == "ellipse"){
                        shape = k;
                    }else if (k == "closest-side" || k == "farthest-side" || k == "closest-corner" || k == "farthest-corner"){
                        sizeKeyword[0] = k;
                        sizeValue[0] = nullptr;
                        sizeSet[0] = true;
                    }else if (k == "cover"){
                        sizeKeyword[0] = "farthest-corner";
                        sizeValue[0] = nullptr;
                        sizeSet[0] = true;
                    }else if (k == "contain"){
                        sizeKeyword[0] = "closest-side";
                        sizeValue[0] = nullptr;
                        sizeSet[0] = true;
                    }else if (isLength(scope.session, *v, true)){
                        // (the first length, or the second one, replaced by any later one)
                        const int at = sizeSet[0] ? 1 : 0;
                        sizeKeyword[at] = v->isToken(TokenType::Number) ? "0px" : std::string_view();
                        sizeValue[at] = v->isToken(TokenType::Number) ? nullptr : v;
                        sizeSet[at] = true;
                    }else{
                        return false;
                    }
                }
                bool first = true;
                auto separate = [&](){
                    if (!first)
                        out += ' ';
                    first = false;
                };
                if (!shape.empty()){
                    separate();
                    out += shape;
                }
                for (int s = 0; s < 2; s++){
                    if (!sizeSet[s])
                        continue;
                    separate();
                    if (sizeValue[s])
                        appendSerialized(out, *sizeValue[s]);
                    else
                        out += sizeKeyword[s];
                }
                if (!position.empty()){
                    Axis x, y;
                    if (!parsePosition(scope.session, position, x, y))
                        return false;
                    separate();
                    out += "at ";
                    appendGradientAxis(out, x, lostOffset);
                    out += ' ';
                    appendGradientAxis(out, y, lostOffset);
                }
            }else if (conic){
                Words position;
                bool at = false;
                bool first = true;
                for (size_t i = 0; i < head.size(); i++){
                    const ComponentValue& v = *head[i];
                    if (at){
                        position.push_back(&v);
                        continue;
                    }
                    if (isIdent(v, "from") && i + 1 < head.size()){
                        const ComponentValue& angle = *head[i + 1];
                        if (!first)
                            out += ' ';
                        first = false;
                        if (angle.isToken(TokenType::Number) && angle.token.number == 0){
                            out += "from 0deg";
                        }else if (isAngle(angle)){
                            out += "from ";
                            appendSerialized(out, angle);
                        }else if (isVar(angle) && scope.context && scope.context->fork(ForkFeature::BackgroundLayers)){
                            // the fork's layers substitute it (a registered <angle> animates the gradient; phase 5)
                            out += "from ";
                            appendSerialized(out, angle);
                        }else{
                            return false;
                        }
                        i++;
                        continue;
                    }
                    if (isIdent(v, "at")){
                        at = true;
                        continue;
                    }
                    return false;
                }
                if (!position.empty()){
                    Axis x, y;
                    if (!parsePosition(scope.session, position, x, y))
                        return false;
                    if (!first)
                        out += ' ';
                    out += "at ";
                    appendGradientAxis(out, x, lostOffset);
                    out += ' ';
                    appendGradientAxis(out, y, lostOffset);
                }
            }
        }
        if (lostOffset)
            say(scope, Severity::Info, codes::ApproximatedMinor, "a gradient position offset from an edge is dropped (the edge is used)");
        if (out.size() > prefixStart)
            out += ", ";
        if (!colorStops(scope, groups, headIsStop || (head.empty() && !interpolation) ? 0 : 1, conic, out, middleColor))
            return false;
        out += ')';
        return true;
    }

    // ---- layers

    enum class Repeat : uint8_t { Repeat, NoRepeat, Space, Round, RepeatX, RepeatY };
    enum class Box : uint8_t { BorderBox, PaddingBox, ContentBox };

    const char* repeatName(Repeat repeat){
        switch (repeat){
            case Repeat::Repeat: return "repeat";
            case Repeat::NoRepeat: return "no-repeat";
            case Repeat::Space: return "space";
            case Repeat::Round: return "round";
            case Repeat::RepeatX: return "repeat-x";
            case Repeat::RepeatY: return "repeat-y";
        }
        return "repeat";
    }

    const char* boxName(Box box){
        switch (box){
            case Box::BorderBox: return "border-box";
            case Box::PaddingBox: return "padding-box";
            case Box::ContentBox: return "content-box";
        }
        return "border-box";
    }

    // a repeat keyword (false when it is none)
    bool repeatOf(std::string_view k, Repeat& repeat){
        if (k == "repeat") repeat = Repeat::Repeat;
        else if (k == "no-repeat") repeat = Repeat::NoRepeat;
        else if (k == "space") repeat = Repeat::Space;
        else if (k == "round") repeat = Repeat::Round;
        else if (k == "repeat-x") repeat = Repeat::RepeatX;
        else if (k == "repeat-y") repeat = Repeat::RepeatY;
        else return false;
        return true;
    }

    // a box keyword; `text`: the text keyword (background-clip: text)
    bool boxOf(std::string_view k, Box& box, bool& text){
        text = false;
        if (k == "border-box") box = Box::BorderBox;
        else if (k == "padding-box") box = Box::PaddingBox;
        else if (k == "content-box") box = Box::ContentBox;
        else if (k == "text") text = true;
        else return false;
        return true;
    }

    // a background-size component: auto, cover, contain, 0 (written 0px) or a value's text
    struct Size {
        enum class Kind : uint8_t { Auto, Cover, Contain, ZeroPx, Value };
        Kind kind = Kind::Auto;
        const ComponentValue* value = nullptr;
    };

    void appendSize(std::string& out, const Size& size){
        switch (size.kind){
            case Size::Kind::Auto: out += "auto"; return;
            case Size::Kind::Cover: out += "cover"; return;
            case Size::Kind::Contain: out += "contain"; return;
            case Size::Kind::ZeroPx: out += "0px"; return;
            case Size::Kind::Value: appendSerialized(out, *size.value); return;
        }
    }

    // (trivially copied: the layers of a declaration are copied from one to the next)
    struct Layer {
        enum class Kind : uint8_t { None, Gradient, Image, Var, Effect };
        Kind kind = Kind::None;
        Text image;                                    // the gradient decorator or the image path; Effect: its typed RCSS once resolved
        const ComponentValue* var = nullptr;           // Var: the var() written as it is
        const ComponentValue* effect = nullptr;        // Effect: the function (a handled image function, phase 5)
        uint32_t effectFile = 0;                       // Effect: the file it is written in
        bool repeatSet = false;                        // a background-repeat was given for the layer
        const ComponentValue* middleColor = nullptr;   // a gradient's middle stop (gradient text)
        Axis x, y;
        bool positionSet = false;
        Size sizeX, sizeY;
        bool sizeSet = false;
        Repeat repeatX = Repeat::Repeat, repeatY = Repeat::Repeat;
        bool fixed = false;                            // background-attachment: fixed (scroll otherwise)
        bool local = false;                            // background-attachment: local (the fork draws it; else scroll)
        Box origin = Box::PaddingBox, clip = Box::BorderBox;
        bool clipText = false;
    };

    struct Layers {
        ArenaList<Layer> layers;
        const ComponentValue* color = nullptr;         // background-color of the shorthand (null = transparent)
        bool colorSet = false;
    };

    // an image-set() candidate for the dp ratio
    const ComponentValue* imageSetCandidate(Session& session, const ComponentValue& function){
        struct Candidate { const ComponentValue* image; double resolution; };
        std::vector<Candidate> candidates;
        for (const ValueRange& group : commaRanges(function.children)){
            const Words w = wordsOf(group);
            if (w.empty())
                continue;
            Candidate c{w[0], 1.0};
            for (size_t i = 1; i < w.size(); i++){
                if (w[i]->isToken(TokenType::Dimension) && unitCategory(w[i]->token.unit) == UnitCategory::Resolution)
                    c.resolution = w[i]->token.number * canonicalFactor(w[i]->token.unit).value_or(1.0);
                else if (w[i]->isFunction("type")){
                    const std::string type = lowerAscii(text(w[i]->children));
                    if (type.find("webp") != std::string::npos || type.find("avif") != std::string::npos)
                        c.resolution = -1;   // a format the engine does not decode
                }
            }
            if (c.resolution > 0)
                candidates.push_back(c);
        }
        if (candidates.empty())
            return nullptr;
        const double ratio = session.options.dpRatio;
        const Candidate* best = nullptr;
        for (const Candidate& c : candidates){
            if (c.resolution >= ratio && (!best || c.resolution < best->resolution || best->resolution < ratio))
                best = &c;
        }
        if (!best){
            for (const Candidate& c : candidates){
                if (!best || c.resolution > best->resolution)
                    best = &c;
            }
        }
        return best->image;
    }

    // (one buffer for the gradients of the thread: a layer keeps a copy of its text)
    std::string& gradientBuffer(){
        static thread_local std::string buffer;
        return buffer;
    }

    // the image of a layer: true when the component is an image (set into layer)
    bool layerImage(LayerScope& scope, const ComponentValue& v, Layer& layer, bool& dropped){
        dropped = false;
        if (isIdent(v, "none")){
            layer.kind = Layer::Kind::None;
            return true;
        }
        if (v.isToken(TokenType::Url) || v.isFunction("url") || v.isFunction("src")){
            std::optional<std::string> url = urlOf(v);
            const SourceRef ref = v.length > 0 ? SourceRef{scope.file, v.offset, v.length} : scope.ref;
            const std::string path = url ? resolveUrlIn(scope.session, *url, scope.file, ref, "image") : std::string();
            if (path.empty()){
                dropped = true;
                return true;
            }
            layer.kind = Layer::Kind::Image;
            layer.image = Text(path);
            return true;
        }
        if (v.kind != ComponentValue::Kind::Function)
            return false;
        const std::string_view name = lowerName(v);
        if (name == "image-set" || name == "-webkit-image-set"){
            const ComponentValue* candidate = imageSetCandidate(scope.session, v);
            if (!candidate){
                say(scope, Severity::Warning, codes::UnsupportedFunction, "image-set() has no candidate the engine can draw; layer dropped",
                    "list a PNG or JPEG candidate");
                dropped = true;
                return true;
            }
            return layerImage(scope, *candidate, layer, dropped);
        }
        if (name == "var"){
            // a static variable is read as what it holds; a dynamic one is written as is
            ValueList replacement;
            if (scope.session.staticVar(v, replacement)){
                const Words w = wordsOf(replacement);
                if (w.size() == 1 && layerImage(scope, *w[0], layer, dropped))
                    return true;
                // a variable holding a color is the final layer's color, not its image
                // (background: var(--bg) url(tile.png) repeat)
                if (w.size() == 1 && isColor(scope.session, *w[0]))
                    return false;
            }
            layer.kind = Layer::Kind::Var;
            layer.var = &v;
            return true;
        }
        // phase 5: an image function a later phase handles (effect())
        if (findImageFunctionHandler(name)){
            layer.kind = Layer::Kind::Effect;
            layer.effect = &v;
            layer.effectFile = scope.file;
            layer.image = Text();
            return true;
        }
        if (name == "-webkit-gradient"){
            say(scope, Severity::Warning, codes::UnsupportedFunction, "the 2008 -webkit-gradient() syntax is not supported; layer dropped",
                "use linear-gradient() or radial-gradient()");
            dropped = true;
            return true;
        }
        if (name == "element" || name == "cross-fade" || name == "-webkit-cross-fade" || name == "paint" || name == "image"){
            // paint(<name>) of an effect the engine has: phase 5's suggestion
            std::string suggestion = "use url() or a gradient";
            if (name == "paint"){
                for (const ComponentValue& arg : v.children){
                    if (arg.isToken(TokenType::Ident)){
                        const std::string effect = jasperengine::htmlui::EffectCompat::paintSuggestion(std::string(arg.token.value.view()));
                        if (!effect.empty())
                            suggestion = effect;
                        break;
                    }
                }
            }
            say(scope, Severity::Warning, codes::UnsupportedFunction, "'" + std::string(name) + "()' is not supported; layer dropped",
                suggestion);
            dropped = true;
            return true;
        }
        std::string_view plain = name;
        for (std::string_view prefix : {std::string_view("-webkit-"), std::string_view("-moz-"), std::string_view("-o-"), std::string_view("-ms-")}){
            if (startsWith(plain, prefix))
                plain = plain.substr(prefix.size());
        }
        if (isGradientName(plain)){
            std::string& out = gradientBuffer();
            out.clear();
            if (!gradient(scope, v, out, &layer.middleColor)){
                say(scope, Severity::Warning, codes::UnsupportedValue, "'" + text(v) + "' cannot be written as a gradient decorator; layer dropped");
                dropped = true;
                return true;
            }
            layer.kind = Layer::Kind::Gradient;
            layer.image = Text(std::string_view(out));
            return true;
        }
        return false;
    }

    // background-repeat values (the first two of `count`)
    void setRepeat(Layer& layer, const Repeat* values, size_t count){
        if (count == 1){
            if (values[0] == Repeat::RepeatX){
                layer.repeatX = Repeat::Repeat;
                layer.repeatY = Repeat::NoRepeat;
            }else if (values[0] == Repeat::RepeatY){
                layer.repeatX = Repeat::NoRepeat;
                layer.repeatY = Repeat::Repeat;
            }else{
                layer.repeatX = layer.repeatY = values[0];
            }
        }else if (count == 2){
            layer.repeatX = values[0];
            layer.repeatY = values[1];
        }
    }

    bool setSize(Session& session, Layer& layer, const Words& w){
        if (w.size() == 1 && (isIdent(*w[0], "cover") || isIdent(*w[0], "contain"))){
            layer.sizeX.kind = layer.sizeY.kind = isIdent(*w[0], "cover") ? Size::Kind::Cover : Size::Kind::Contain;
            layer.sizeX.value = layer.sizeY.value = nullptr;
            layer.sizeSet = true;
            return true;
        }
        if (w.empty() || w.size() > 2)
            return false;
        Size values[2];
        for (size_t i = 0; i < w.size(); i++){
            if (isIdent(*w[i], "auto"))
                values[i].kind = Size::Kind::Auto;
            else if (isLength(session, *w[i], true) || isVar(*w[i])){
                if (w[i]->isToken(TokenType::Number)){
                    values[i].kind = Size::Kind::ZeroPx;
                }else{
                    values[i].kind = Size::Kind::Value;
                    values[i].value = w[i];
                }
            }else
                return false;
        }
        layer.sizeX = values[0];
        layer.sizeY = values[1];
        layer.sizeSet = !(values[0].kind == Size::Kind::Auto && values[1].kind == Size::Kind::Auto);
        return true;
    }

    // one layer of the background / mask shorthand
    bool parseLayer(LayerScope& scope, const ValueRange& group, bool last, Layer& layer, Layers& out, bool& dropped){
        dropped = false;
        const Words w = wordsOf(group);
        Words position;
        Repeat repeat[2];
        size_t repeats = 0;
        Box boxes[2] = {Box::BorderBox, Box::BorderBox};
        bool boxText[2] = {false, false};
        size_t boxCount = 0;
        bool imageSet = false;
        for (size_t i = 0; i < w.size(); i++){
            const ComponentValue& v = *w[i];
            const std::string_view k = identView(v);
            if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                // position / size
                Words size;
                size_t j = i + 1;
                while (j < w.size() && (isLength(scope.session, *w[j], true) || isIdent(*w[j], "auto") || isIdent(*w[j], "cover") ||
                                        isIdent(*w[j], "contain")) && size.size() < 2){
                    size.push_back(w[j]);
                    j++;
                }
                if (position.empty() || !setSize(scope.session, layer, size))
                    return false;
                i = j - 1;
                continue;
            }
            if (!imageSet){
                bool imageDropped = false;
                if (layerImage(scope, v, layer, imageDropped)){
                    imageSet = true;
                    if (imageDropped)
                        dropped = true;
                    continue;
                }
            }
            Repeat r;
            if (repeatOf(k, r)){
                if (repeats < 2)
                    repeat[repeats] = r;
                repeats++;
                continue;
            }
            if (k == "scroll" || k == "fixed" || k == "local"){
                const bool native = scope.context && scope.context->fork(ForkFeature::BackgroundLayers);
                if (k == "local" && !native)
                    say(scope, Severity::Info, codes::ApproximatedMinor, "'background-attachment: local' is drawn as scroll");
                layer.fixed = k == "fixed";
                layer.local = k == "local";
                continue;
            }
            Box box;
            bool isText = false;
            if (boxOf(k, box, isText)){
                if (boxCount < 2){
                    boxes[boxCount] = box;
                    boxText[boxCount] = isText;
                }
                boxCount++;
                continue;
            }
            if (k == "left" || k == "right" || k == "top" || k == "bottom" || k == "center" ||
                isLength(scope.session, v, true) || (isVar(v) && imageSet)){
                position.push_back(&v);
                continue;
            }
            if (last && (isColor(scope.session, v) || isVar(v)) && !out.colorSet){
                out.color = &v;
                out.colorSet = true;
                continue;
            }
            return false;
        }
        if (!position.empty()){
            if (!parsePosition(scope.session, position, layer.x, layer.y))
                return false;
            layer.positionSet = true;
        }
        if (repeats > 0){
            setRepeat(layer, repeat, repeats);
            layer.repeatSet = true;
        }
        if (boxCount >= 1){
            if (boxText[0])
                layer.clipText = true;
            else
                layer.origin = boxes[0];
            if (boxCount >= 2){
                if (boxText[1])
                    layer.clipText = true;
                else
                    layer.clip = boxes[1];
            }else if (!boxText[0]){
                layer.clip = boxes[0];
            }
        }
        return true;
    }

    // the decorator list of the layers, appended to `out` (nothing when none)
    void appendDecorators(LayerScope& scope, const ArenaList<Layer>& layers, std::string& out){
        bool reportedPattern = false;
        bool any = false;
        auto separate = [&](){
            if (any)
                out += ", ";
            any = true;
        };
        for (const Layer& layer : layers){
            switch (layer.kind){
                case Layer::Kind::None:
                    break;
                case Layer::Kind::Var:
                    separate();
                    appendSerialized(out, *layer.var);
                    break;
                case Layer::Kind::Effect:
                    // the typed RCSS of the effect (resolveEffects), with its paint area
                    if (!layer.image.empty()){
                        separate();
                        out += layer.image.view();
                    }
                    break;
                case Layer::Kind::Gradient: {
                    const bool fullBox = !layer.sizeSet && (!layer.positionSet || (atZero(layer.x) && atZero(layer.y)));
                    if (!fullBox && !reportedPattern){
                        reportedPattern = true;
                        say(scope, Severity::Warning, codes::ApproximatedVisible,
                            "a gradient layer with a background-size or background-position is drawn once over the whole box",
                            "draw the pattern with an image");
                    }
                    separate();
                    out += layer.image.view();
                    out += ' ';
                    out += boxName(layer.clip);
                    break;
                }
                case Layer::Kind::Image:
                    separate();
                    out += "html-layer(";
                    appendString(out, layer.image);
                    out += ' ';
                    appendSize(out, layer.sizeX);
                    out += ' ';
                    appendSize(out, layer.sizeY);
                    out += ' ';
                    appendAxis(out, layer.x);
                    out += ' ';
                    appendAxis(out, layer.y);
                    out += ' ';
                    out += repeatName(layer.repeatX);
                    out += ' ';
                    out += repeatName(layer.repeatY);
                    out += ' ';
                    out += boxName(layer.origin);
                    out += ' ';
                    out += boxName(layer.clip);
                    out += ' ';
                    out += layer.fixed ? "fixed" : "scroll";
                    out += ')';
                    break;
            }
        }
    }

    // ---- longhands: lists applied to the image layers (CSS repetition)

    bool isBackgroundFamily(std::string_view name, bool mask){
        if (mask){
            if (name == "clip-path")
                return true;
            for (std::string_view prefix : {std::string_view("mask"), std::string_view("-webkit-mask")}){
                if (name == prefix || (startsWith(name, prefix) && name.size() > prefix.size() && name[prefix.size()] == '-'))
                    return true;
            }
            return false;
        }
        return name == "background" || name == "background-image" || name == "background-position" || name == "background-position-x" ||
               name == "background-position-y" || name == "background-size" || name == "background-repeat" || name == "background-origin" ||
               name == "background-clip" || name == "background-attachment" || name == "-webkit-background-clip" ||
               name == "-webkit-background-size" || name == "-webkit-background-origin";
    }

    std::string_view plainName(std::string_view name){
        for (std::string_view prefix : {std::string_view("-webkit-"), std::string_view("-moz-")}){
            if (startsWith(name, prefix))
                return name.substr(prefix.size());
        }
        return name;
    }

    // applies one declaration of the family to the layers; false when the value cannot be read
    bool applyDeclaration(LayerScope& scope, std::string_view rawName, const ValueList& value, Layers& state){
        std::string_view name = plainName(rawName);
        std::string maskName;
        if (scope.mask && startsWith(name, "mask-")){
            maskName = "background-" + std::string(name.substr(5));
            name = maskName;
        }else if (scope.mask && name == "mask"){
            name = "background";
        }
        const ArenaList<ValueRange> groups = commaRanges(value);
        if (name == "background"){
            Layers fresh;
            fresh.layers.reserve(groups.size());
            for (size_t i = 0; i < groups.size(); i++){
                Layer layer;
                bool dropped = false;
                if (!parseLayer(scope, groups[i], i + 1 == groups.size(), layer, fresh, dropped)){
                    if (scope.mask){
                        // mask-mode, mask-composite parts of the shorthand
                        say(scope, Severity::Info, codes::UnsupportedValue, "mask shorthand parts beyond image, position, size, repeat, origin and clip are dropped");
                    }
                    return false;
                }
                if (!dropped)
                    fresh.layers.push_back(layer);
            }
            state.layers = std::move(fresh.layers);
            state.color = fresh.color;
            state.colorSet = fresh.colorSet;
            if (!state.colorSet){
                state.colorSet = true;
                state.color = nullptr;
            }
            return true;
        }
        if (name == "background-image"){
            const ArenaList<Layer> previous = state.layers;
            state.layers.clear();
            for (size_t i = 0; i < groups.size(); i++){
                Layer layer = i < previous.size() ? previous[i] : (previous.empty() ? Layer() : previous[i % previous.size()]);
                const Words w = wordsOf(groups[i]);
                bool dropped = false;
                if (w.size() != 1 || !layerImage(scope, *w[0], layer, dropped))
                    return false;
                if (!dropped)
                    state.layers.push_back(layer);
            }
            return true;
        }
        // the per-layer longhands repeat over the layers
        if (state.layers.empty())
            state.layers.push_back(Layer());
        static const ComponentValue kCenter = literalIdent("center");
        for (size_t i = 0; i < state.layers.size(); i++){
            const Words w = wordsOf(groups[i % groups.size()]);
            Layer& layer = state.layers[i];
            if (name == "background-position"){
                if (!parsePosition(scope.session, w, layer.x, layer.y))
                    return false;
                layer.positionSet = true;
            }else if (name == "background-position-x" || name == "background-position-y"){
                // one axis: read as a position with the other axis centered
                const bool horizontal = name == "background-position-x";
                Words position;
                position.reserve(w.size() + 1);
                if (!horizontal)
                    position.push_back(&kCenter);
                for (const ComponentValue* v : w)
                    position.push_back(v);
                if (horizontal)
                    position.push_back(&kCenter);
                Axis x, y;
                if (w.empty() || !parsePosition(scope.session, position, x, y))
                    return false;
                (horizontal ? layer.x : layer.y) = horizontal ? x : y;
                layer.positionSet = true;
            }else if (name == "background-size"){
                if (!setSize(scope.session, layer, w))
                    return false;
            }else if (name == "background-repeat"){
                Repeat repeat[2];
                size_t count = 0;
                for (const ComponentValue* v : w){
                    Repeat r;
                    if (!repeatOf(identView(*v), r))
                        return false;
                    if (count < 2)
                        repeat[count] = r;
                    count++;
                }
                setRepeat(layer, repeat, count);
                layer.repeatSet = true;
            }else if (name == "background-origin"){
                Box box;
                bool isText = false;
                if (w.size() != 1 || !boxOf(identView(*w[0]), box, isText) || isText)
                    return false;
                layer.origin = box;
            }else if (name == "background-clip"){
                Box box;
                bool isText = false;
                if (w.size() != 1 || !boxOf(identView(*w[0]), box, isText))
                    return false;
                if (isText)
                    layer.clipText = true;
                else
                    layer.clip = box;
            }else if (name == "background-attachment"){
                const std::string_view k = w.size() == 1 ? identView(*w[0]) : std::string_view();
                if (k != "scroll" && k != "fixed" && k != "local")
                    return false;
                layer.fixed = k == "fixed";
                layer.local = k == "local";
            }else{
                return false;
            }
        }
        return true;
    }

    bool hasImageDeclaration(const FlatRule& rule, bool mask){
        for (const Declaration& d : rule.declarations){
            const std::string_view n = plainName(d.name);
            if (!mask && (n == "background" || n == "background-image"))
                return true;
            if (mask && (n == "mask" || n == "mask-image"))
                return true;
        }
        return false;
    }

    // the normalized value of a declaration of another rule (base rules)
    ValueList normalized(Session& session, const Declaration& d){
        NormalizeContext context;
        context.options = &session.options;
        context.resolveVar = [&session](const ComponentValue& var, ValueList& out){ return session.staticVar(var, out); };
        NormalizeResult result = ValueNormalizer::normalize(d.value, context);
        return result.ok ? trimWhitespace(result.value) : ValueList();
    }

    // ---- gradient text

    bool transparentFill(Session& session, const FlatRule& rule){
        for (const char* property : {"-webkit-text-fill-color", "color"}){
            std::optional<ValueList> value = ruleValue(session, rule, property);
            if (value && value->size() == 1 && (*value)[0].isToken(TokenType::Hash)){
                const Text& hex = (*value)[0].token.value;
                if (hex.size() == 8 && hex.compare(6, 2, "00") == 0)
                    return true;
            }
        }
        return false;
    }

    LayerScope scopeOf(DeclContext& context, bool mask){
        LayerScope scope{context.session, context.declaration.ref.file, context.declaration.ref, mask, &context, false};
        return scope;
    }

    // (one buffer for the decorator lists of the thread: the emitted value is a copy)
    std::string& decoratorBuffer(){
        static thread_local std::string buffer;
        return buffer;
    }

    // ---- effect layers (phase 5): an effect() layer is written as the typed RCSS its image
    // function handler gives (EffectCompat); with background-clip: text and a transparent text
    // fill, the fork fills the text with it (-jfx-text-fill, ForkFeature::TextFillFilter)

    // what the rule says of its layers beyond one declaration
    struct RuleLayerFacts {
        bool size = false, position = false, repeat = false;
        std::vector<std::string> blend;     // background-blend-mode per layer (the list repeats)
        std::vector<bool> textClip;         // background-clip: text per layer (the list repeats)
    };

    RuleLayerFacts ruleLayerFacts(Session& session, const FlatRule* rule, bool mask){
        RuleLayerFacts facts;
        if (!rule)
            return facts;
        session.memoTaint++;   // (reads the rule's other declarations)
        for (const Declaration& d : rule->declarations){
            std::string_view name = plainName(d.name);
            std::string mapped;
            if (mask && startsWith(name, "mask-")){
                mapped = "background-" + std::string(name.substr(5));
                name = mapped;
            }else if (!mask && startsWith(name, "mask")){
                continue;
            }
            if (name == "background-size")
                facts.size = true;
            else if (name == "background-position" || name == "background-position-x" || name == "background-position-y")
                facts.position = true;
            else if (name == "background-repeat")
                facts.repeat = true;
            else if (!mask && name == "background-blend-mode"){
                facts.blend.clear();
                for (const ValueRange& group : commaRanges(d.value)){
                    const Words w = wordsOf(group);
                    facts.blend.push_back(w.size() == 1 ? std::string(identView(*w[0])) : std::string("?"));
                }
            }else if (!mask && name == "background-clip"){
                facts.textClip.clear();
                for (const ValueRange& group : commaRanges(d.value)){
                    const Words w = wordsOf(group);
                    facts.textClip.push_back(w.size() == 1 && identView(*w[0]) == "text");
                }
            }
        }
        return facts;
    }

    // the rule (or its base rule) makes the text fill transparent
    bool ruleTransparentFill(Session& session, const FlatRule* rule){
        if (!rule)
            return false;
        if (transparentFill(session, *rule))
            return true;
        return BaseRules::find(session, *rule, [&](const FlatRule& candidate){ return transparentFill(session, candidate); }) != nullptr;
    }

    // Writes the typed RCSS of the effect layers (layer.image); a dropped effect is no layer.
    // With textFill, the effect layers clipped to text go into `fills` in their filter form,
    // bottom layer first, and leave the list (native: a `none` placeholder keeps the indices).
    void resolveEffects(LayerScope& scope, ArenaList<Layer>& layers, bool mask, bool native, const RuleLayerFacts& facts, bool textFill,
                        std::vector<std::string>* fills){
        DiagnosticSink quiet(scope.session.files);
        DiagnosticSink& sink = (scope.context || scope.toSink) ? *scope.session.sink : quiet;
        for (size_t i = 0; i < layers.size(); i++){
            Layer& layer = layers[i];
            if (layer.kind != Layer::Kind::Effect || !layer.effect)
                continue;
            const ImageFunctionHandler* handler = findImageFunctionHandler(layer.effect->name().view());
            if (!handler){
                layer.kind = Layer::Kind::None;
                continue;
            }
            ImageLayerContext context;
            context.session = &scope.session;
            context.declaration = scope.context;
            context.function = layer.effect;
            context.file = layer.effectFile;
            context.ref = layer.effect->length > 0 ? SourceRef{layer.effectFile, layer.effect->offset, layer.effect->length} : scope.ref;
            context.property = mask ? "mask-image" : "background-image";
            context.layerIndex = i;
            context.clip = boxName(layer.clip);
            context.sizeSet = layer.sizeSet || facts.size;
            context.positionSet = layer.positionSet || facts.position;
            context.repeatSet = layer.repeatSet || facts.repeat;
            context.blendMode = facts.blend.empty() ? std::string() : facts.blend[i % facts.blend.size()];
            context.native = native;
            const bool clipped = layer.clipText || (!facts.textClip.empty() && facts.textClip[i % facts.textClip.size()]);
            context.textFill = textFill && clipped && !mask && fills;
            std::optional<std::string> rcss = (*handler)(context, sink);
            if (!rcss){
                layer.kind = Layer::Kind::None;
                layer.image = Text();
                continue;
            }
            if (context.textFill){
                fills->insert(fills->begin(), *rcss);
                layer.clipText = false;
                layer.image = Text();
                if (!native)
                    layer.kind = Layer::Kind::None;
                continue;
            }
            layer.image = Text(*rcss);
        }
    }

    // writes the text fill of gradient text with effect layers
    void emitTextFill(DeclContext& context, const std::vector<std::string>& fills){
        std::string list;
        for (const std::string& fill : fills)
            list += (list.empty() ? "" : " ") + fill;
        context.emit("-jfx-text-fill", list);
        context.emit("color", "#ffffffff");
        context.reportAs(Severity::Info, codes::EffectTextFill,
                         "background-clip: text fills the content with the effect: the transparent text color is drawn white for it",
                         "all of the element's content is filled, not only its glyphs");
    }

    // ---- the fork's layers (ForkFeature::BackgroundLayers, phase 3 p3-paint): each declaration is written as the
    // native longhands it sets, the values normalized (url() resolved, gradients as the fork's gradient functions)

    // one longhand list of the layers
    std::string nativeList(const ArenaList<Layer>& layers, const char* longhand){
        std::string out;
        for (size_t i = 0; i < layers.size(); i++){
            const Layer& layer = layers[i];
            if (i > 0)
                out += ", ";
            const std::string_view name = longhand;
            if (name == "image"){
                switch (layer.kind){
                    case Layer::Kind::None: out += "none"; break;
                    case Layer::Kind::Var: appendSerialized(out, *layer.var); break;
                    case Layer::Kind::Gradient: out += layer.image.view(); break;
                    case Layer::Kind::Effect: out += layer.image.empty() ? std::string_view("none") : layer.image.view(); break;
                    case Layer::Kind::Image:
                        out += "url(";
                        appendString(out, layer.image);
                        out += ")";
                        break;
                }
            }else if (name == "position-x"){
                appendAxis(out, layer.x);
            }else if (name == "position-y"){
                appendAxis(out, layer.y);
            }else if (name == "size"){
                if (layer.sizeX.kind == Size::Kind::Cover || layer.sizeX.kind == Size::Kind::Contain){
                    appendSize(out, layer.sizeX);
                }else{
                    appendSize(out, layer.sizeX);
                    out += ' ';
                    appendSize(out, layer.sizeY);
                }
            }else if (name == "repeat"){
                out += repeatName(layer.repeatX);
                out += ' ';
                out += repeatName(layer.repeatY);
            }else if (name == "origin"){
                out += boxName(layer.origin);
            }else if (name == "clip"){
                out += boxName(layer.clip);
            }else if (name == "attachment"){
                out += layer.fixed ? "fixed" : layer.local ? "local" : "scroll";
            }
        }
        return out;
    }

    void rewriteLayersNative(DeclContext& context, bool mask){
        LayerScope scope = scopeOf(context, mask);
        const std::string prefix = mask ? "mask-" : "background-";
        std::string_view name = plainName(context.name);
        std::string maskName;
        if (mask && startsWith(name, "mask-")){
            maskName = "background-" + std::string(name.substr(5));
            name = maskName;
        }else if (mask && name == "mask"){
            name = "background";
        }

        // a whole-value var() is the fork's to resolve (typed: its url()s resolved)
        if (context.value.size() == 1 && isVar(context.value[0])){
            const std::string target = mask && context.name == "clip-path" ? std::string("mask-image") : std::string(plainName(context.name));
            std::optional<TypedVar> typed = context.name == "clip-path" ? std::nullopt : wholeVar(context, "native-layers");
            if (typed)
                context.emit(target, typed->reference);
            else
                context.emit(target, context.valueText());
            return;
        }

        if (mask && context.name == "clip-path"){
            const Words w = wordsOf(context.value);
            if (w.size() == 1 && isIdent(*w[0], "none")){
                context.emit("mask-image", "none");
                return;
            }
            std::optional<std::string> shape = clipPathShape(context, context.declaration);
            if (shape)
                context.emit("mask-image", *shape);
            return;
        }

        // the layers the declaration describes: the shorthand all of them, a longhand one list
        Layers state;
        const ArenaList<ValueRange> groups = commaRanges(context.value);
        if (name != "background" && name != "background-image"){
            // a list longhand: one layer per item
            for (size_t i = 0; i < groups.size(); i++)
                state.layers.push_back(Layer());
        }
        if (!applyDeclaration(scope, context.name, context.value, state)){
            context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped",
                             "check the background syntax");
            context.dropped = true;
            return;
        }

        // effect layers (phase 5): their typed RCSS; with gradient text, the fork fills the text with them
        if (name == "background" || name == "background-image"){
            std::vector<std::string> fills;
            const RuleLayerFacts facts = ruleLayerFacts(context.session, context.rule, mask);
            const bool textFill = !mask && context.fork(ForkFeature::TextFillFilter) && ruleTransparentFill(context.session, context.rule);
            resolveEffects(scope, state.layers, mask, true, facts, textFill, &fills);
            if (!fills.empty())
                emitTextFill(context, fills);
        }else if (name == "background-clip" && !mask && context.rule && ruleHasEffectTextFill(context.session, *context.rule)){
            // the effect layers fill the text (-jfx-text-fill): their clip is the border box
            for (Layer& layer : state.layers)
                layer.clipText = false;
        }

        // gradient text: background-clip: text with a transparent text fill, as without the fork
        bool clipText = false;
        std::string middle;
        for (const Layer& layer : state.layers){
            if (layer.clipText){
                clipText = true;
                if (middle.empty())
                    middle = layer.kind == Layer::Kind::Gradient ? (layer.middleColor ? text(*layer.middleColor) : std::string()) : "#ffffffff";
            }
        }
        if (clipText && !mask){
            const FlatRule* base = context.rule ? BaseRules::find(context.session, *context.rule, [&](const FlatRule& candidate){
                return transparentFill(context.session, candidate);
            }) : nullptr;
            if (context.rule && (transparentFill(context.session, *context.rule) || base)){
                // (a background-clip declaration alone: the colour of the gradient the rule, or its base rule, declares)
                if (name == "background-clip"){
                    middle.clear();
                    for (const FlatRule* rule : {static_cast<const FlatRule*>(context.rule), base}){
                        if (!rule || !middle.empty())
                            continue;
                        for (const char* property : {"background", "background-image"}){
                            const std::optional<ValueList> value = ruleValue(context.session, *rule, property);
                            Layers images;
                            if (!value || !applyDeclaration(scope, property, *value, images))
                                continue;
                            for (const Layer& layer : images.layers){
                                if (layer.kind == Layer::Kind::Gradient && layer.middleColor){
                                    middle = text(*layer.middleColor);
                                    break;
                                }
                            }
                        }
                    }
                }
                if (middle.empty())
                    middle = "#ffffffff";
                context.reportAs(Severity::Warning, codes::ApproximatedVisible, "gradient text is drawn in a solid color (" + middle + ")",
                                 "use a solid text color");
                if (name == "background")
                    context.emit("background-color", state.colorSet && state.color ? Text(text(*state.color)) : Text::view("transparent"));
                context.emit("background-image", "none");
                context.emit("color", middle);
                return;
            }
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'background-clip: text' without a transparent text color is dropped");
            if (name == "background-clip")
                return;
        }

        auto emitList = [&](const char* longhand){
            // (every layer dropped: no images)
            if (state.layers.empty()){
                if (std::string_view(longhand) == "image")
                    context.emit(prefix + longhand, "none");
                return;
            }
            context.emit(prefix + longhand, nativeList(state.layers, longhand));
        };
        if (name == "background"){
            if (!mask){
                if (state.colorSet && state.color)
                    context.emit("background-color", text(*state.color));
                else
                    context.emit("background-color", "transparent");
            }
            for (const char* longhand : {"image", "position-x", "position-y", "size", "repeat"})
                emitList(longhand);
            if (!mask){
                for (const char* longhand : {"origin", "clip", "attachment"})
                    emitList(longhand);
            }
            return;
        }
        if (name == "background-image")
            emitList("image");
        else if (name == "background-position"){
            emitList("position-x");
            emitList("position-y");
        }else if (name == "background-position-x")
            emitList("position-x");
        else if (name == "background-position-y")
            emitList("position-y");
        else if (name == "background-size")
            emitList("size");
        else if (name == "background-repeat")
            emitList("repeat");
        else if (!mask && name == "background-origin")
            emitList("origin");
        else if (!mask && name == "background-clip")
            emitList("clip");
        else if (!mask && name == "background-attachment")
            emitList("attachment");
        else{
            // mask-origin, mask-clip, mask-mode, mask-composite: the fork has no such properties
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + context.name + "' has no effect (the mask's box is its border box)");
            context.dropped = true;
        }
    }

    // the family of the rule, merged at its last declaration (and with the base rule)
    void rewriteLayers(DeclContext& context, bool mask){
        if (context.fork(ForkFeature::BackgroundLayers)){
            rewriteLayersNative(context, mask);
            return;
        }
        LayerScope scope = scopeOf(context, mask);
        Layers state;
        if (context.rule && !context.keyframe){
            const Declaration* last = nullptr;
            for (const Declaration& d : context.rule->declarations){
                if (isBackgroundFamily(d.name, mask) && reachesRewrite(context.session, d, context.rule->declarations))
                    last = &d;
            }
            if (last && last->ref != context.declaration.ref)
                return;
            if (!hasImageDeclaration(*context.rule, mask)){
                const bool onlyClipPath = [&](){
                    for (const Declaration& d : context.rule->declarations){
                        if (isBackgroundFamily(d.name, mask) && d.name != "clip-path")
                            return false;
                    }
                    return true;
                }();
                if (!onlyClipPath){
                    const FlatRule* base = BaseRules::find(context.session, *context.rule, [mask](const FlatRule& candidate){
                        return hasImageDeclaration(candidate, mask);
                    });
                    if (base){
                        LayerScope quiet = scope;
                        quiet.context = nullptr;
                        quiet.file = base->ref.file;
                        for (const Declaration& d : base->declarations){
                            if (isBackgroundFamily(d.name, mask) && d.name != "clip-path" && reachesRewrite(context.session, d, base->declarations))
                                applyDeclaration(quiet, d.name, normalized(context.session, d), state);
                        }
                        context.reportAs(Severity::Info, codes::MergedFromBaseRule, std::string(mask ? "mask" : "background") +
                                         " longhands are merged with the layers of the base rule at " + BaseRules::location(context.session, *base));
                    }else{
                        context.reportAs(Severity::Warning, codes::LonghandWithoutBase, "'" + context.name + "' without " +
                                         (mask ? "a mask image" : "a background image") + " in the rule or a base rule is dropped",
                                         std::string("set ") + (mask ? "mask-image" : "background-image") + " in the same rule");
                        context.dropped = true;
                        return;
                    }
                }
            }
            bool applied = false, attempted = false;
            for (const Declaration& d : context.rule->declarations){
                if (!isBackgroundFamily(d.name, mask) || d.name == "clip-path" || !reachesRewrite(context.session, d, context.rule->declarations))
                    continue;
                ValueList storage;
                const ValueList& value = d.ref == context.declaration.ref ? context.value : (storage = normalized(context.session, d));
                if (value.size() == 1 && isVar(value[0])){
                    applied = true;
                    continue;   // a whole-value var() is handled below for its own declaration
                }
                attempted = true;
                if (applyDeclaration(scope, d.name, value, state))
                    applied = true;
                else
                    context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(value) + "' is not supported; dropped",
                                     "check the background syntax");
            }
            if (attempted && !applied && !(mask && context.name == "clip-path")){
                context.dropped = true;
                return;
            }
        }else{
            if (!(context.value.size() == 1 && isVar(context.value[0])) && !applyDeclaration(scope, context.name, context.value, state)){
                context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped",
                                 "check the background syntax");
                context.dropped = true;
                return;
            }
        }

        // a whole-value var() (the last declaration of the family)
        const std::string_view plain = plainName(context.name);
        if (context.value.size() == 1 && isVar(context.value[0]) && (plain == "background" || plain == "background-image" ||
                                                                      plain == "mask" || plain == "mask-image")){
            std::optional<TypedVar> typed = wholeVar(context, mask ? "mask" : "decorator");
            if (!typed){
                context.emit(mask ? "mask-image" : "decorator", context.valueText());
                return;
            }
            if (typed->kind == "color"){
                context.emit("background-color", typed->reference);
                companions::emitBackgroundLayers(context, std::string_view());
            }else{
                if (!mask && plain == "background")
                    context.emit("background-color", "transparent");
                if (mask)
                    context.emit("mask-image", typed->reference);
                else
                    companions::emitBackgroundLayers(context, typed->reference);
            }
            return;
        }

        // effect layers (phase 5): their typed RCSS with their paint area; with gradient text,
        // the fork fills the text with them
        {
            std::vector<std::string> fills;
            const RuleLayerFacts facts = ruleLayerFacts(context.session, context.rule, mask);
            const bool textFill = !mask && context.fork(ForkFeature::TextFillFilter) && ruleTransparentFill(context.session, context.rule);
            resolveEffects(scope, state.layers, mask, false, facts, textFill, &fills);
            if (!fills.empty())
                emitTextFill(context, fills);
        }

        // gradient text: background-clip: text with a transparent text fill
        bool clipText = false;
        std::string middle;
        for (const Layer& layer : state.layers){
            if (layer.clipText){
                clipText = true;
                if (middle.empty())
                    middle = layer.kind == Layer::Kind::Gradient ? (layer.middleColor ? text(*layer.middleColor) : std::string()) : "#ffffffff";
            }
        }
        if (clipText && !mask){
            const FlatRule* base = context.rule ? BaseRules::find(context.session, *context.rule, [&](const FlatRule& candidate){
                return transparentFill(context.session, candidate);
            }) : nullptr;
            if (context.rule && (transparentFill(context.session, *context.rule) || base)){
                if (middle.empty())
                    middle = "#ffffffff";
                context.reportAs(Severity::Warning, codes::ApproximatedVisible, "gradient text is drawn in a solid color (" + middle + ")",
                                 "use a solid text color");
                if (state.colorSet)
                    context.emit("background-color", state.color ? Text(text(*state.color)) : Text::view("transparent"));
                companions::emitBackgroundLayers(context, std::string_view());
                context.emit("color", middle);
                return;
            }
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'background-clip: text' without a transparent text color is dropped");
            for (Layer& layer : state.layers)
                layer.clipText = false;
        }

        std::string& decorators = decoratorBuffer();
        decorators.clear();
        appendDecorators(scope, state.layers, decorators);
        if (mask){
            // clip-path shapes join the mask list
            std::string shapes;
            if (context.rule){
                for (const Declaration& d : context.rule->declarations){
                    if (d.name != "clip-path")
                        continue;
                    std::optional<std::string> shape = clipPathShape(context, d);
                    if (shape)
                        shapes = *shape;
                }
            }
            if (!shapes.empty()){
                if (!decorators.empty())
                    decorators += ", ";
                decorators += shapes;
            }
            context.emit("mask-image", decorators.empty() ? Text::view("none") : Text(std::string_view(decorators)));
            return;
        }
        if (state.colorSet){
            if (state.color){
                // (the color's text after the decorators, in the same buffer)
                const size_t at = decorators.size();
                appendSerialized(decorators, *state.color);
                context.emit("background-color", Text(std::string_view(decorators).substr(at)));
                decorators.resize(at);
            }else{
                context.emit("background-color", Text::view("transparent"));
            }
        }
        companions::emitBackgroundLayers(context, decorators);
    }

    // ---- var() families

    std::string classifyLayers(Session& session, const ValueList& value, bool mask){
        const Words w = wordsOf(value);
        if (w.size() == 1 && isColor(session, *w[0]))
            return mask ? std::string() : "color";
        LayerScope scope{session, 0, SourceRef(), mask, nullptr, false};
        Layers state;
        if (!applyDeclaration(scope, "background", value, state))
            return std::string();
        if (state.colorSet && state.color)
            return std::string();   // layers and a color: two RCSS properties
        return "layers";
    }

    std::optional<std::string> transformLayers(Session& session, const ValueList& value, const std::string& kind,
                                               SourceRef ref, bool mask){
        if (kind == "color")
            return text(value);
        LayerScope scope{session, ref.file, ref, mask, nullptr, true};
        Layers state;
        if (!applyDeclaration(scope, "background", value, state))
            return std::nullopt;
        resolveEffects(scope, state.layers, mask, false, RuleLayerFacts(), false, nullptr);
        std::string decorators;
        appendDecorators(scope, state.layers, decorators);
        return decorators.empty() ? std::string(companions::kNoLayer) : decorators;
    }

    std::string classifyDecorator(Session& session, const ValueList& value){ return classifyLayers(session, value, false); }
    std::string classifyMask(Session& session, const ValueList& value){ return classifyLayers(session, value, true); }
    std::optional<std::string> transformDecorator(Session& session, const ValueList& value, const std::string& kind, SourceRef ref){
        return transformLayers(session, value, kind, ref, false);
    }
    std::optional<std::string> transformMask(Session& session, const ValueList& value, const std::string& kind, SourceRef ref){
        return transformLayers(session, value, kind, ref, true);
    }

    // the fork's layers (BackgroundLayers) read a variable as the CSS value it is, its url()s resolved for the file that
    // declares it (phase 3, p3-paint)
    std::string classifyNativeLayers(Session& session, const ValueList& value){
        // (a colour is a background too: one kind)
        const Words w = wordsOf(value);
        if (w.size() == 1 && isColor(session, *w[0]))
            return "layers";
        LayerScope scope{session, 0, SourceRef(), false, nullptr, false};
        Layers state;
        return applyDeclaration(scope, "background", value, state) ? "layers" : std::string();
    }
    std::optional<std::string> transformNativeLayers(Session& session, const ValueList& value, const std::string&, SourceRef ref){
        std::string out;
        size_t layerIndex = 0;
        for (const ComponentValue& v : value){
            if (v.isToken(TokenType::Comma))
                layerIndex++;
            // phase 5: an effect() layer in its typed form (a dropped one is no image)
            if (v.kind == ComponentValue::Kind::Function){
                if (const ImageFunctionHandler* handler = findImageFunctionHandler(v.name().view())){
                    ImageLayerContext context;
                    context.session = &session;
                    context.function = &v;
                    context.file = ref.file;
                    context.ref = v.length > 0 ? SourceRef{ref.file, v.offset, v.length} : ref;
                    context.property = "background-image";
                    context.layerIndex = layerIndex;
                    context.native = true;
                    std::optional<std::string> rcss = (*handler)(context, *session.sink);
                    out += rcss ? *rcss : std::string("none");
                    continue;
                }
            }
            if (v.isToken(TokenType::Url) || v.isFunction("url")){
                std::optional<std::string> url = urlOf(v);
                const std::string path = url ? resolveUrlIn(session, *url, ref.file, ref, "image") : std::string();
                if (path.empty())
                    return std::nullopt;
                out += "url(\"" + path + "\")";
                continue;
            }
            appendSerialized(out, v);
        }
        const size_t begin = out.find_first_not_of(" \t\n");
        const size_t end = out.find_last_not_of(" \t\n");
        return begin == std::string::npos ? std::string() : out.substr(begin, end - begin + 1);
    }

}

const VarFamily jasperengine::htmlui::compat::kNativeLayersFamily = {"native-layers", classifyNativeLayers, transformNativeLayers};
const VarFamily jasperengine::htmlui::compat::kDecoratorFamily = {"decorator", classifyDecorator, transformDecorator};
const VarFamily jasperengine::htmlui::compat::kMaskFamily = {"mask", classifyMask, transformMask};

void jasperengine::htmlui::compat::rewriteBackground(DeclContext& context){
    rewriteLayers(context, false);
}

void jasperengine::htmlui::compat::rewriteBackgroundLonghand(DeclContext& context){
    rewriteLayers(context, false);
}

void jasperengine::htmlui::compat::rewriteMaskImage(DeclContext& context){
    rewriteLayers(context, true);
}

void jasperengine::htmlui::compat::rewriteClipPath(DeclContext& context){
    const Words w = wordsOf(context.value);
    if (w.size() == 1 && isIdent(*w[0], "none")){
        if (!context.rule)
            context.emit("mask-image", "none");
        else
            rewriteLayers(context, true);
        return;
    }
    if (!context.rule){
        std::optional<std::string> shape = clipPathShape(context, context.declaration);
        if (shape)
            context.emit("mask-image", *shape);
        return;
    }
    rewriteLayers(context, true);
}

bool jasperengine::htmlui::compat::ruleHasEffectLayer(Session& session, const FlatRule& rule){
    session.memoTaint++;
    for (const Declaration& d : rule.declarations){
        const std::string_view name = plainName(d.name);
        if (name != "background" && name != "background-image")
            continue;
        for (const ComponentValue& v : d.value){
            if (v.kind == ComponentValue::Kind::Function && findImageFunctionHandler(v.name().view()))
                return true;
        }
    }
    return false;
}

bool jasperengine::htmlui::compat::ruleHasEffectTextFill(Session& session, const FlatRule& rule){
    if (!session.fork(ForkFeature::TextFillFilter) || !ruleHasEffectLayer(session, rule) || !ruleTransparentFill(session, &rule))
        return false;
    for (const Declaration& d : rule.declarations){
        const std::string_view name = plainName(d.name);
        if (name != "background" && name != "background-clip")
            continue;
        for (const ComponentValue& v : d.value){
            if (v.isToken(TokenType::Ident) && equalsNoCase(v.token.value, "text"))
                return true;
        }
    }
    return false;
}
