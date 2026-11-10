// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlShapeDecorator.h"

#include "htmlui/compat/css/CssSerialize.h"

#include "Log.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/PropertyParser.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleValue.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

namespace {

    std::string num(float value){
        return formatNumber(value, 4);
    }

    struct Resolver {
        Rml::Element* element;
        float width, height;

        // a length-percentage against `base`; false when not one
        bool length(const std::string& token, float base, float& out) const{
            Length length;
            if (!parseLength(token, length))
                return false;
            out = resolve(element, length, base);
            return true;
        }
    };

    bool fail(std::string* error, const std::string& message){
        if (error)
            *error = message;
        return false;
    }

    // CSS <position> (1 to 4 values) in the reference box
    bool parsePosition(const std::vector<std::string>& tokens, const Resolver& r, float& x, float& y, std::string* error){
        x = r.width * 0.5f;
        y = r.height * 0.5f;
        if (tokens.empty())
            return true;
        const auto keywordX = [](const std::string& t){ return t == "left" || t == "right"; };
        const auto keywordY = [](const std::string& t){ return t == "top" || t == "bottom"; };
        const auto keywordValue = [&](const std::string& t, bool horizontal) -> float {
            if (t == "center")
                return (horizontal ? r.width : r.height) * 0.5f;
            if (t == "left" || t == "top")
                return 0.0f;
            return horizontal ? r.width : r.height;
        };
        std::vector<std::string> t;
        for (const std::string& token : tokens)
            t.push_back(lower(token));

        if (t.size() == 1){
            if (keywordY(t[0])){
                y = keywordValue(t[0], false);
            }else if (keywordX(t[0]) || t[0] == "center"){
                x = keywordValue(t[0], true);
            }else if (!r.length(t[0], r.width, x)){
                return fail(error, "position '" + tokens[0] + "' is not understood");
            }
            return true;
        }
        if (t.size() == 2){
            std::string first = t[0], second = t[1];
            if (keywordY(first) || keywordX(second))
                std::swap(first, second);
            if (keywordY(first) || keywordX(second))
                return fail(error, "position '" + tokens[0] + " " + tokens[1] + "' is not understood");
            if (keywordX(first) || first == "center")
                x = keywordValue(first, true);
            else if (!r.length(first, r.width, x))
                return fail(error, "position '" + tokens[0] + "' is not understood");
            if (keywordY(second) || second == "center")
                y = keywordValue(second, false);
            else if (!r.length(second, r.height, y))
                return fail(error, "position '" + tokens[1] + "' is not understood");
            return true;
        }
        // 3 or 4 values: edge offsets ("right 10px bottom 20px", "left 10px top")
        size_t i = 0;
        bool haveX = false, haveY = false;
        while (i < t.size()){
            const std::string edge = t[i++];
            float offset = 0.0f;
            const bool horizontal = keywordX(edge) || (edge == "center" && !haveX);
            const bool vertical = keywordY(edge) || (edge == "center" && haveX);
            if (!horizontal && !vertical)
                return fail(error, "position edge '" + edge + "' is not left, right, top, bottom or center");
            const float base = horizontal ? r.width : r.height;
            if (edge != "center" && i < t.size() && !keywordX(t[i]) && !keywordY(t[i]) && t[i] != "center"){
                if (!r.length(t[i], base, offset))
                    return fail(error, "position offset '" + t[i] + "' is not a length or percentage");
                i++;
            }
            float value = keywordValue(edge, horizontal);
            if (edge == "right" || edge == "bottom")
                value -= offset;
            else
                value += offset;
            if (horizontal){
                x = value;
                haveX = true;
            }else{
                y = value;
                haveY = true;
            }
        }
        (void)haveY;
        return true;
    }

    // border-radius shorthand values ("10px 20% / 5px") for a w x h rectangle:
    // [corner][0 = rx, 1 = ry], corners top-left, top-right, bottom-right, bottom-left
    bool parseRadii(const std::vector<std::string>& tokens, const Resolver& r, float w, float h, std::array<std::array<float, 2>, 4>& radii,
                    std::string* error){
        std::vector<std::string> horizontal, vertical;
        bool slash = false;
        for (const std::string& token : tokens){
            if (token == "/"){
                if (slash)
                    return fail(error, "radius has two '/'");
                slash = true;
                continue;
            }
            (slash ? vertical : horizontal).push_back(token);
        }
        if (horizontal.empty() || horizontal.size() > 4 || vertical.size() > 4 || (slash && vertical.empty()))
            return fail(error, "radius needs 1 to 4 values on each side of '/'");
        if (vertical.empty())
            vertical = horizontal;
        const auto expand = [](std::vector<std::string> values){
            // top-left, top-right, bottom-right, bottom-left
            if (values.size() == 1) values = {values[0], values[0], values[0], values[0]};
            else if (values.size() == 2) values = {values[0], values[1], values[0], values[1]};
            else if (values.size() == 3) values = {values[0], values[1], values[2], values[1]};
            return values;
        };
        horizontal = expand(horizontal);
        vertical = expand(vertical);
        for (int i = 0; i < 4; i++){
            if (!r.length(horizontal[i], w, radii[i][0]) || !r.length(vertical[i], h, radii[i][1]))
                return fail(error, "radius '" + horizontal[i] + "' is not a length or percentage");
            radii[i][0] = std::max(0.0f, radii[i][0]);
            radii[i][1] = std::max(0.0f, radii[i][1]);
        }
        // CSS: scale every radius down so adjacent ones fit their side
        float factor = 1.0f;
        const float sums[4] = {radii[0][0] + radii[1][0], radii[1][1] + radii[2][1], radii[2][0] + radii[3][0], radii[3][1] + radii[0][1]};
        const float sides[4] = {w, h, w, h};
        for (int i = 0; i < 4; i++){
            if (sums[i] > sides[i] && sums[i] > 0.0f)
                factor = std::min(factor, sides[i] / sums[i]);
        }
        for (auto& corner : radii){
            corner[0] *= factor;
            corner[1] *= factor;
        }
        return true;
    }

    std::string roundedRectPath(float x, float y, float w, float h, const std::array<std::array<float, 2>, 4>& radii){
        const auto& tl = radii[0];
        const auto& tr = radii[1];
        const auto& br = radii[2];
        const auto& bl = radii[3];
        std::string d = "M" + num(x + tl[0]) + " " + num(y);
        d += "H" + num(x + w - tr[0]);
        d += "A" + num(tr[0]) + " " + num(tr[1]) + " 0 0 1 " + num(x + w) + " " + num(y + tr[1]);
        d += "V" + num(y + h - br[1]);
        d += "A" + num(br[0]) + " " + num(br[1]) + " 0 0 1 " + num(x + w - br[0]) + " " + num(y + h);
        d += "H" + num(x + bl[0]);
        d += "A" + num(bl[0]) + " " + num(bl[1]) + " 0 0 1 " + num(x) + " " + num(y + h - bl[1]);
        d += "V" + num(y + tl[1]);
        d += "A" + num(tl[0]) + " " + num(tl[1]) + " 0 0 1 " + num(x + tl[0]) + " " + num(y);
        d += "Z";
        return d;
    }

    // splits "a b round c d" at the `round` keyword
    void splitRound(const std::vector<std::string>& tokens, std::vector<std::string>& before, std::vector<std::string>& radii){
        bool round = false;
        for (const std::string& token : tokens){
            if (!round && lower(token) == "round"){
                round = true;
                continue;
            }
            (round ? radii : before).push_back(token);
        }
    }

    bool isBoxKeyword(const std::string& token, Rml::BoxArea& box){
        const std::string value = lower(token);
        if (value == "border-box" || value == "margin-box" || value == "stroke-box" || value == "view-box")
            box = Rml::BoxArea::Border;
        else if (value == "padding-box")
            box = Rml::BoxArea::Padding;
        else if (value == "content-box" || value == "fill-box")
            box = Rml::BoxArea::Content;
        else
            return false;
        return true;
    }

    std::vector<std::string> expandShapeCorners(std::vector<std::string> values){
        if (values.empty()) values = {"0"};
        if (values.size() == 1) return {values[0], values[0], values[0], values[0]};
        if (values.size() == 2) return {values[0], values[1], values[0], values[1]};
        if (values.size() == 3) return {values[0], values[1], values[2], values[1]};
        return values;
    }

    struct ShapeAnimationParts {
        std::string name;
        std::string fillRule = "nonzero";
        std::vector<std::string> values;
        std::vector<std::string> position;
        std::vector<std::string> radii;
    };

    bool shapeAnimationParts(const BasicShape& shape, ShapeAnimationParts& out){
        const size_t open = shape.text.find('(');
        out.name = lower(shape.text.substr(0, open));
        auto tokens = tokenize(shape.text.substr(open + 1, shape.text.size() - open - 2));
        for (auto& token : tokens) token = lower(token);
        if (out.name == "inset"){
            std::vector<std::string> radiusTokens;
            splitRound(tokens, out.values, radiusTokens);
            out.values = expandShapeCorners(out.values);
            auto slash = std::find(radiusTokens.begin(), radiusTokens.end(), "/");
            std::vector<std::string> x(radiusTokens.begin(), slash);
            std::vector<std::string> y = slash == radiusTokens.end() ? x : std::vector<std::string>(slash + 1, radiusTokens.end());
            x = expandShapeCorners(x);
            y = expandShapeCorners(y);
            out.radii = x;
            out.radii.insert(out.radii.end(), y.begin(), y.end());
            return out.values.size() == 4 && out.radii.size() == 8;
        }
        if (out.name == "circle" || out.name == "ellipse"){
            auto at = std::find(tokens.begin(), tokens.end(), "at");
            out.values.assign(tokens.begin(), at);
            if (at != tokens.end()) out.position.assign(at + 1, tokens.end());
            if (out.values.empty()) out.values.assign(out.name == "circle" ? 1 : 2, "closest-side");
            return out.values.size() == (out.name == "circle" ? 1u : 2u);
        }
        if (out.name == "polygon"){
            size_t start = 0;
            if (tokens.size() > 1 && (tokens[0] == "nonzero" || tokens[0] == "evenodd")){
                out.fillRule = tokens[0];
                start = 2;
            }
            for (size_t i = start; i < tokens.size(); i++)
                if (tokens[i] != ",") out.values.push_back(tokens[i]);
            return out.values.size() >= 6 && out.values.size() % 2 == 0;
        }
        return false;
    }

    class ShapeAnimationValue : public Rml::StyleValue {
    public:
        explicit ShapeAnimationValue(BasicShape shape): shape(std::move(shape)) {}

        Rml::String ToString() const override{
            const char* box = shape.box == Rml::BoxArea::Content ? " content-box" : shape.box == Rml::BoxArea::Padding ? " padding-box" : "";
            return serializeString(shape.text + box);
        }

        bool Equals(const Rml::StyleValue& other) const override{
            const auto& to = static_cast<const ShapeAnimationValue&>(other);
            return shape.text == to.shape.text && shape.box == to.shape.box;
        }

        Rml::SharedPtr<const Rml::StyleValue> Interpolate(const Rml::StyleValue& other, float alpha, Rml::Element& element) const override{
            const auto& to = static_cast<const ShapeAnimationValue&>(other);
            const Rml::Rectanglef box = boxRectangle(&element, shape.box);
            BasicShape mixed;
            if (!shape.interpolate(to.shape, alpha, box.Width(), box.Height(), &element, mixed))
                return nullptr;
            return std::make_shared<ShapeAnimationValue>(std::move(mixed));
        }

        Rml::Units GetUnitDependencies() const override{
            return shape.unitDependencies();
        }

        BasicShape shape;
    };

    class ShapeAnimationParser : public Rml::PropertyParser {
    public:
        bool ParseValue(Rml::Property& property, const Rml::String& value, const Rml::ParameterMap&) const override{
            BasicShape shape;
            std::string error;
            if (!BasicShape::parse(value, shape, &error)){
                Log::warn("[HTML UI] html-shape(): %s", error.c_str());
                return false;
            }
            property = Rml::Property(Rml::StyleValuePtr(std::make_shared<ShapeAnimationValue>(std::move(shape))), Rml::Unit::STYLE_VALUE);
            return true;
        }
    };

}

// ---------------------------------------------------------------- BasicShape

struct BasicShape::Interpolation {
    BasicShape from, to;
    float alpha;
};

Rml::Units BasicShape::unitDependencies() const{
    if (interpolation)
        return interpolation->from.unitDependencies() | interpolation->to.unitDependencies();
    ShapeAnimationParts parts;
    if (!shapeAnimationParts(*this, parts)) return Rml::Unit::UNKNOWN;
    Rml::Units units = Rml::Unit::UNKNOWN;
    for (const auto* tokens : {&parts.values, &parts.position, &parts.radii}){
        for (const auto& token : *tokens){
            Length length;
            if (parseLength(token, length)) units = units | length.unit;
        }
    }
    return units;
}

bool BasicShape::parse(const std::string& input, BasicShape& shape, std::string* error){
    shape = BasicShape();
    std::vector<std::string> tokens = tokenize(unquote(input));
    std::string function;
    for (const std::string& token : tokens){
        Rml::BoxArea box;
        if (token.find('(') != std::string::npos){
            if (!function.empty())
                return fail(error, "two shapes in '" + input + "'");
            function = token;
        }else if (isBoxKeyword(token, box)){
            shape.box = box;
        }else{
            return fail(error, "'" + token + "' is not a shape or a reference box");
        }
    }
    if (function.empty())
        return fail(error, "no shape in '" + input + "'");
    shape.text = function;
    // a 1x1 conversion checks the arguments
    std::string check;
    if (shape.toSvg(100.0f, 100.0f, nullptr, &check).empty())
        return fail(error, check);
    return true;
}

bool BasicShape::interpolate(const BasicShape& to, float alpha, float width, float height, Rml::Element* element, BasicShape& result) const{
    BasicShape fromResolved, toResolved;
    if (!resolveInterpolation(width, height, element, fromResolved) || !to.resolveInterpolation(width, height, element, toResolved))
        return false;
    ShapeAnimationParts a, b;
    if (box != to.box || !shapeAnimationParts(fromResolved, a) || !shapeAnimationParts(toResolved, b) || a.name != b.name ||
        a.fillRule != b.fillRule || a.values.size() != b.values.size())
        return false;
    const Resolver resolver{element, width, height};
    const auto mix = [alpha](float from, float toValue){ return from + (toValue - from) * alpha; };
    const auto length = [&](const std::string& from, const std::string& toValue, float basis, float& value){
        float x, y;
        if (!resolver.length(from, basis, x) || !resolver.length(toValue, basis, y)) return false;
        value = mix(x, y);
        return true;
    };
    result.box = box;
    if (a.name == "inset"){
        std::array<float, 4> offsets{};
        result.text = "inset(";
        for (size_t i = 0; i < 4; i++){
            if (!length(a.values[i], b.values[i], i % 2 == 0 ? height : width, offsets[i])) return false;
            result.text += (i ? " " : "") + num(offsets[i]) + "px";
        }
        const float shapeWidth = std::max(0.0f, width - offsets[1] - offsets[3]);
        const float shapeHeight = std::max(0.0f, height - offsets[0] - offsets[2]);
        result.text += " round";
        for (size_t i = 0; i < 8; i++){
            float radius;
            if (!length(a.radii[i], b.radii[i], i < 4 ? shapeWidth : shapeHeight, radius)) return false;
            result.text += (i == 4 ? " / " : " ") + num(std::max(0.0f, radius)) + "px";
        }
    }else if (a.name == "circle" || a.name == "ellipse"){
        float ax, ay, bx, by;
        if (!parsePosition(a.position, resolver, ax, ay, nullptr) || !parsePosition(b.position, resolver, bx, by, nullptr)) return false;
        result.text = a.name + "(";
        for (size_t i = 0; i < a.values.size(); i++){
            float radius;
            const float basis = a.name == "circle" ? std::hypot(width, height) / std::sqrt(2.0f) : i == 0 ? width : height;
            if (length(a.values[i], b.values[i], basis, radius)){
                result.text += (i ? " " : "") + num(std::max(0.0f, radius)) + "px";
            }else{
                return false;
            }
        }
        result.text += " at " + num(mix(ax, bx)) + "px " + num(mix(ay, by)) + "px";
    }else{
        result.text = "polygon(" + a.fillRule + ", ";
        for (size_t i = 0; i < a.values.size(); i++){
            float coordinate;
            if (!length(a.values[i], b.values[i], i % 2 == 0 ? width : height, coordinate)) return false;
            result.text += (i ? i % 2 == 0 ? ", " : " " : "") + num(coordinate) + "px";
        }
    }
    result.text += ")";
    // Animation sampling precedes layout. Keep the component endpoints so GenerateElementData
    // resolves percentages again against the final box, including a size animated in this frame.
    result.interpolation = std::make_shared<Interpolation>(Interpolation{*this, to, alpha});
    return true;
}

bool BasicShape::resolveInterpolation(float width, float height, Rml::Element* element, BasicShape& result) const{
    if (!interpolation){
        result = *this;
        return true;
    }
    if (!interpolation->from.interpolate(interpolation->to, interpolation->alpha, width, height, element, result))
        return false;
    result.interpolation.reset();
    return true;
}

std::string BasicShape::toSvg(float width, float height, Rml::Element* element, std::string* error) const{
    if (interpolation){
        BasicShape resolved;
        if (!resolveInterpolation(width, height, element, resolved))
            return std::string();
        return resolved.toSvg(width, height, element, error);
    }
    if (!(width > 0.0f) || !(height > 0.0f)){
        fail(error, "the reference box is empty");
        return std::string();
    }
    const size_t open = text.find('(');
    if (open == std::string::npos || text.back() != ')'){
        fail(error, "'" + text + "' is not a function");
        return std::string();
    }
    const std::string name = lower(trim(text.substr(0, open)));
    const std::string arguments = text.substr(open + 1, text.size() - open - 2);
    const Resolver r{element, width, height};
    std::string shapeMarkup;
    std::string fillRule = "nonzero";

    std::vector<std::string> tokens = tokenize(arguments);
    if (name == "inset"){
        std::vector<std::string> offsets, radiusTokens;
        splitRound(tokens, offsets, radiusTokens);
        if (offsets.empty() || offsets.size() > 4){
            fail(error, "inset() needs 1 to 4 offsets");
            return std::string();
        }
        std::vector<std::string> o = offsets;
        if (o.size() == 1) o = {o[0], o[0], o[0], o[0]};
        else if (o.size() == 2) o = {o[0], o[1], o[0], o[1]};
        else if (o.size() == 3) o = {o[0], o[1], o[2], o[1]};
        float top, right, bottom, left;
        if (!r.length(o[0], height, top) || !r.length(o[1], width, right) || !r.length(o[2], height, bottom) || !r.length(o[3], width, left)){
            fail(error, "inset() offsets must be lengths or percentages");
            return std::string();
        }
        // offsets that overlap are scaled down proportionally (CSS Shapes 1)
        if (left + right > width && left + right > 0.0f){
            const float f = width / (left + right);
            left *= f;
            right *= f;
        }
        if (top + bottom > height && top + bottom > 0.0f){
            const float f = height / (top + bottom);
            top *= f;
            bottom *= f;
        }
        const float w = width - left - right, h = height - top - bottom;
        std::array<std::array<float, 2>, 4> radii{};
        if (!radiusTokens.empty() && !parseRadii(radiusTokens, r, w, h, radii, error))
            return std::string();
        shapeMarkup = "<path d=\"" + roundedRectPath(left, top, w, h, radii) + "\"";
    }else if (name == "xywh" || name == "rect"){
        std::vector<std::string> values, radiusTokens;
        splitRound(tokens, values, radiusTokens);
        if (values.size() != 4){
            fail(error, name + "() needs 4 values");
            return std::string();
        }
        float x, y, w, h;
        if (name == "xywh"){
            if (!r.length(values[0], width, x) || !r.length(values[1], height, y) || !r.length(values[2], width, w) ||
                !r.length(values[3], height, h) || w < 0.0f || h < 0.0f){
                fail(error, "xywh() needs lengths or percentages (non-negative sizes)");
                return std::string();
            }
        }else{
            // rect(top right bottom left), offsets from the top and left edges; auto = the edge
            float top = 0.0f, right = width, bottom = height, left = 0.0f;
            const auto edge = [&](const std::string& token, float base, float automatic, float& out){
                if (lower(token) == "auto"){
                    out = automatic;
                    return true;
                }
                return r.length(token, base, out);
            };
            if (!edge(values[0], height, 0.0f, top) || !edge(values[1], width, width, right) || !edge(values[2], height, height, bottom) ||
                !edge(values[3], width, 0.0f, left)){
                fail(error, "rect() needs lengths, percentages or auto");
                return std::string();
            }
            x = left;
            y = top;
            w = std::max(0.0f, right - left);
            h = std::max(0.0f, bottom - top);
        }
        std::array<std::array<float, 2>, 4> radii{};
        if (!radiusTokens.empty() && !parseRadii(radiusTokens, r, w, h, radii, error))
            return std::string();
        shapeMarkup = "<path d=\"" + roundedRectPath(x, y, w, h, radii) + "\"";
    }else if (name == "circle" || name == "ellipse"){
        std::vector<std::string> radiusTokens, positionTokens;
        bool at = false;
        for (const std::string& token : tokens){
            if (!at && lower(token) == "at"){
                at = true;
                continue;
            }
            (at ? positionTokens : radiusTokens).push_back(token);
        }
        float cx, cy;
        if ((at && positionTokens.empty()) || !parsePosition(positionTokens, r, cx, cy, error))
            return std::string();
        const bool circle = name == "circle";
        if (radiusTokens.size() > (circle ? 1u : 2u)){
            fail(error, name + "() has too many radii");
            return std::string();
        }
        const auto sideRadius = [&](const std::string& token, bool horizontal, float& out) -> bool {
            const std::string value = lower(token);
            const float dx[2] = {std::fabs(cx), std::fabs(width - cx)};
            const float dy[2] = {std::fabs(cy), std::fabs(height - cy)};
            if (value == "closest-side" || value == "farthest-side"){
                const bool closest = value == "closest-side";
                if (circle){
                    const float values[4] = {dx[0], dx[1], dy[0], dy[1]};
                    out = closest ? *std::min_element(values, values + 4) : *std::max_element(values, values + 4);
                }else{
                    const float* d = horizontal ? dx : dy;
                    out = closest ? std::min(d[0], d[1]) : std::max(d[0], d[1]);
                }
                return true;
            }
            const float base = circle ? std::sqrt(width * width + height * height) / std::sqrt(2.0f) : (horizontal ? width : height);
            return r.length(token, base, out) && out >= 0.0f;
        };
        float rx = 0.0f, ry = 0.0f;
        if (circle){
            if (!sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[0], true, rx)){
                fail(error, "circle() radius must be a length, percentage, closest-side or farthest-side");
                return std::string();
            }
            shapeMarkup = "<circle cx=\"" + num(cx) + "\" cy=\"" + num(cy) + "\" r=\"" + num(rx) + "\"";
        }else{
            if (radiusTokens.size() == 1){
                fail(error, "ellipse() needs two radii or none");
                return std::string();
            }
            if (!sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[0], true, rx) ||
                !sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[1], false, ry)){
                fail(error, "ellipse() radii must be lengths, percentages, closest-side or farthest-side");
                return std::string();
            }
            shapeMarkup = "<ellipse cx=\"" + num(cx) + "\" cy=\"" + num(cy) + "\" rx=\"" + num(rx) + "\" ry=\"" + num(ry) + "\"";
        }
    }else if (name == "polygon" || name == "path"){
        // an optional fill rule before the first comma
        size_t start = 0;
        if (!tokens.empty() && (lower(tokens[0]) == "nonzero" || lower(tokens[0]) == "evenodd")){
            fillRule = lower(tokens[0]);
            if (tokens.size() < 2 || tokens[1] != ","){
                fail(error, name + "() needs a comma after the fill rule");
                return std::string();
            }
            start = 2;
        }
        if (name == "path"){
            if (tokens.size() != start + 1){
                fail(error, "path() needs one string of path data");
                return std::string();
            }
            const std::string d = unquote(tokens[start]);
            if (d.empty()){
                fail(error, "path() has no path data");
                return std::string();
            }
            std::string escaped;
            for (char c : d)
                escaped += c == '"' ? std::string("&quot;") : c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : std::string(1, c);
            shapeMarkup = "<path d=\"" + escaped + "\"";
        }else{
            std::string points;
            std::vector<std::string> pair;
            size_t count = 0;
            const auto flush = [&]() -> bool {
                if (pair.size() != 2)
                    return false;
                float x, y;
                if (!r.length(pair[0], width, x) || !r.length(pair[1], height, y))
                    return false;
                points += (points.empty() ? "" : " ") + num(x) + "," + num(y);
                pair.clear();
                count++;
                return true;
            };
            for (size_t i = start; i < tokens.size(); i++){
                if (tokens[i] == ","){
                    if (!flush()){
                        fail(error, "polygon() points must be pairs of lengths or percentages");
                        return std::string();
                    }
                }else{
                    pair.push_back(tokens[i]);
                }
            }
            if (!flush() || count < 3){
                fail(error, "polygon() needs at least 3 points of lengths or percentages");
                return std::string();
            }
            shapeMarkup = "<polygon points=\"" + points + "\"";
        }
    }else{
        fail(error, "'" + name + "()' is not a basic shape (inset, circle, ellipse, polygon, path, xywh, rect)");
        return std::string();
    }

    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(width) + "\" height=\"" + num(height) + "\" viewBox=\"0 0 " +
           num(width) + " " + num(height) + "\">" + shapeMarkup + " fill=\"#ffffff\" fill-rule=\"" + fillRule + "\"/></svg>";
}

// ---------------------------------------------------------------- decorator

struct HtmlShapeDecorator::Data {
    Rml::Geometry geometry;
    Rml::CallbackTexture texture;
    std::string svg;
    Rml::Vector2i pixels{0, 0};
    Rml::Vector2f offset{0, 0};
};

HtmlShapeDecorator::HtmlShapeDecorator(BasicShape shape): shape(std::move(shape)) {}

HtmlShapeDecorator::~HtmlShapeDecorator() = default;

Rml::DecoratorDataHandle HtmlShapeDecorator::GenerateElementData(Rml::Element* element, Rml::BoxArea /*paint_area*/) const{
    Data* data = new Data();
    Rml::RenderManager* renderManager = element->GetRenderManager();
    const Rml::Rectanglef box = boxRectangle(element, shape.box);
    if (!renderManager || box.Width() <= 0.0f || box.Height() <= 0.0f)
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);

    std::string error;
    data->svg = shape.toSvg(box.Width(), box.Height(), element, &error);
    if (data->svg.empty()){
        reportOnce(element, codes::UnsupportedValue, "clip-path shape '" + shape.text + "': " + error, "");
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);
    }
    const float scale = rasterScale(element);
    data->pixels = Rml::Vector2i(std::max(1, std::min(8192, (int)std::lround(box.Width() * scale))),
                                 std::max(1, std::min(8192, (int)std::lround(box.Height() * scale))));
    std::shared_ptr<const std::vector<uint8_t>> rgba = rasterizeCached(data->svg, data->pixels.x, data->pixels.y);
    if (!rgba)
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);
    const Rml::Vector2i pixels = data->pixels;
    data->texture = renderManager->MakeCallbackTexture([rgba, pixels](const Rml::CallbackTextureInterface& textureInterface) -> bool {
        return textureInterface.GenerateTexture({reinterpret_cast<const Rml::byte*>(rgba->data()), rgba->size()}, pixels);
    });

    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, box.TopLeft(), box.Size(), Rml::Colourb(255, 255, 255, 255).ToPremultiplied(element->GetDrawOpacity()),
                                     Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
    data->geometry = renderManager->MakeGeometry(std::move(mesh));
    return reinterpret_cast<Rml::DecoratorDataHandle>(data);
}

void HtmlShapeDecorator::ReleaseElementData(Rml::DecoratorDataHandle element_data) const{
    delete reinterpret_cast<Data*>(element_data);
}

void HtmlShapeDecorator::RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const{
    Data* data = reinterpret_cast<Data*>(element_data);
    if (!data || !data->geometry)
        return;
    data->geometry.Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), data->texture);
}

std::string HtmlShapeDecorator::svgOf(Rml::DecoratorDataHandle element_data){
    const Data* data = reinterpret_cast<const Data*>(element_data);
    return data ? data->svg : std::string();
}

Rml::Vector2i HtmlShapeDecorator::pixelSizeOf(Rml::DecoratorDataHandle element_data){
    const Data* data = reinterpret_cast<const Data*>(element_data);
    return data ? data->pixels : Rml::Vector2i(0, 0);
}

// ---------------------------------------------------------------- instancer

HtmlShapeDecoratorInstancer::HtmlShapeDecoratorInstancer(){
    shapeParser = std::make_unique<ShapeAnimationParser>();
    Rml::StyleSheetSpecification::RegisterParser("html-shape", shapeParser.get());
    shapeId = RegisterProperty("shape", "\"inset(0)\"").AddParser("html-shape").GetId();
    RegisterShorthand("decorator", "shape", Rml::ShorthandType::FallThrough);
}

HtmlShapeDecoratorInstancer::~HtmlShapeDecoratorInstancer() = default;

Rml::SharedPtr<Rml::Decorator> HtmlShapeDecoratorInstancer::InstanceDecorator(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties,
                                                                              const Rml::DecoratorInstancerInterface& /*instancer_interface*/){
    const Rml::Property* property = properties.GetProperty(shapeId);
    if (!property || property->unit != Rml::Unit::STYLE_VALUE)
        return nullptr;
    const Rml::StyleValuePtr value = property->Get<Rml::StyleValuePtr>();
    if (!value) return nullptr;
    return std::make_shared<HtmlShapeDecorator>(static_cast<const ShapeAnimationValue&>(*value).shape);
}

bool HtmlShapeDecoratorInstancer::InterpolatesAsImageLayer() const{
    return true;
}
