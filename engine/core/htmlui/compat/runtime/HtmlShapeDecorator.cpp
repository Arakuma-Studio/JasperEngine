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
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

namespace {

    std::string num(float value){
        return formatNumber(value, 4);
    }

    // calc(), min(), max() and clamp() of lengths, percentages (of `base`) and numbers: + - * / and
    // parentheses (CSS Values 4, 10), to px
    struct ShapeCalc {
        std::string_view text;
        size_t i;
        float base;
        Rml::Element* element;

        struct Value {
            float value = 0.0f;
            bool length = false;   // false: a <number>
        };

        void skip(){
            while (i < text.size() && std::isspace((unsigned char)text[i]))
                i++;
        }
        bool term(Value& out){
            skip();
            if (i >= text.size())
                return false;
            // min(a, b, ...), max(a, b, ...), clamp(min, value, max): of one type
            for (const std::string_view name : {std::string_view("min("), std::string_view("max("), std::string_view("clamp(")}){
                if (text.substr(i, name.size()) != name)
                    continue;
                i += name.size();
                std::vector<Value> arguments;
                while (true){
                    Value argument;
                    if (!sum(argument))
                        return false;
                    arguments.push_back(argument);
                    skip();
                    if (i < text.size() && text[i] == ','){
                        i++;
                        continue;
                    }
                    if (i >= text.size() || text[i] != ')')
                        return false;
                    i++;
                    break;
                }
                for (const Value& argument : arguments){
                    if (argument.length != arguments.front().length)
                        return false;
                }
                out = arguments.front();
                if (name == "clamp("){
                    if (arguments.size() != 3)
                        return false;
                    out.value = std::max(arguments[0].value, std::min(arguments[1].value, arguments[2].value));
                    return true;
                }
                for (const Value& argument : arguments)
                    out.value = name == "min(" ? std::min(out.value, argument.value) : std::max(out.value, argument.value);
                return true;
            }
            const bool nested = text.substr(i, 5) == "calc(";
            if (nested || text[i] == '('){
                i += nested ? 5 : 1;
                if (!sum(out))
                    return false;
                skip();
                if (i >= text.size() || text[i] != ')')
                    return false;
                i++;
                return true;
            }
            const size_t begin = i;
            if (text[i] == '+' || text[i] == '-')
                i++;
            while (i < text.size() && (std::isalnum((unsigned char)text[i]) || text[i] == '.' || text[i] == '%'))
                i++;
            Length length;
            if (!parseLength(text.substr(begin, i - begin), length, true))
                return false;
            out.length = !length.isNumber();
            out.value = out.length ? resolve(element, length, base) : length.value;
            return true;
        }
        bool product(Value& out){
            if (!term(out))
                return false;
            while (true){
                skip();
                if (i >= text.size() || (text[i] != '*' && text[i] != '/'))
                    return true;
                const char op = text[i++];
                Value right;
                if (!term(right))
                    return false;
                if (op == '*'){
                    if (out.length && right.length)
                        return false;
                    out.value *= right.value;
                    out.length = out.length || right.length;
                }else{
                    if (right.length || right.value == 0.0f)
                        return false;
                    out.value /= right.value;
                }
            }
        }
        bool sum(Value& out){
            if (!product(out))
                return false;
            while (true){
                skip();
                // (an operator needs white space on both sides: '-' right after a value would start the next one)
                if (i >= text.size() || (text[i] != '+' && text[i] != '-') || i + 1 >= text.size() || !std::isspace((unsigned char)text[i + 1]))
                    return true;
                const char op = text[i++];
                Value right;
                if (!product(right))
                    return false;
                // a zero number stands for a zero length (as a bare 0 does)
                if (out.length != right.length && !(out.length ? right.value == 0.0f : out.value == 0.0f))
                    return false;
                out.value = op == '+' ? out.value + right.value : out.value - right.value;
                out.length = out.length || right.length;
            }
        }
    };

    struct Resolver {
        Rml::Element* element;
        float width, height;
        Rml::Unit coefficientUnit = Rml::Unit::UNKNOWN;

        // a length-percentage against `base`, calc() included; false when not one
        bool length(const std::string& token, float base, float& out) const{
            const std::string text = lower(trim(token));
            if (text.compare(0, 5, "calc(") == 0 || text.compare(0, 4, "min(") == 0 || text.compare(0, 4, "max(") == 0 ||
                text.compare(0, 6, "clamp(") == 0){
                ShapeCalc calc{text, 0, base, element};
                ShapeCalc::Value value;
                if (!calc.term(value))
                    return false;
                calc.skip();
                if (calc.i != text.size() || (!value.length && value.value != 0.0f))
                    return false;
                out = value.value;
                return true;
            }
            Length length;
            if (!parseLength(token, length))
                return false;
            if (coefficientUnit != Rml::Unit::UNKNOWN)
                out = length.unit == coefficientUnit ? length.value * (length.isPercent() ? base / 100.0f : 1.0f) : 0.0f;
            else
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
            // CSS Shapes resolves percentages against the reference box, before reducing
            // overlapping corner curves to fit the inset rectangle.
            if (!r.length(horizontal[i], r.width, radii[i][0]) || !r.length(vertical[i], r.height, radii[i][1]))
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

        Rml::SharedPtr<const Rml::StyleValue> Interpolate(const Rml::StyleValue& other, float alpha, Rml::Element& /*element*/) const override{
            const auto& to = static_cast<const ShapeAnimationValue&>(other);
            BasicShape mixed;
            if (!shape.interpolate(to.shape, alpha, mixed))
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

namespace {

    // A calc() that is a sum of lengths and percentages, each times a number (CSS Values 4: what a
    // shape's coefficients interpolate as, unit by unit). min(), max() and clamp() are not linear:
    // such a shape animates discretely.
    struct ShapeLinearCalc {
        std::string_view text;
        size_t i = 0;
        struct Term {
            std::vector<Length> lengths;
            float number = 0.0f;
            bool isNumber = false;
        };

        void skip(){
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) i++;
        }
        bool eat(char c){
            skip();
            if (i < text.size() && text[i] == c){
                i++;
                return true;
            }
            return false;
        }
        static void scale(Term& term, float factor){
            for (Length& length : term.lengths) length.value *= factor;
            term.number *= factor;
        }
        static void add(Term& into, const Term& from, float sign){
            for (const Length& length : from.lengths){
                auto found = std::find_if(into.lengths.begin(), into.lengths.end(), [&](const Length& item){ return item.unit == length.unit; });
                if (found == into.lengths.end()) into.lengths.push_back({length.value * sign, length.unit});
                else found->value += length.value * sign;
            }
            into.number += from.number * sign;
        }
        bool factor(Term& out){
            skip();
            if (eat('-')){
                if (!factor(out)) return false;
                scale(out, -1.0f);
                return true;
            }
            if (text.compare(i, 5, "calc(") == 0){
                i += 4;
            }
            if (eat('(')) return sum(out) && eat(')');
            const size_t start = i;
            while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '(' && text[i] != ')' &&
                   text[i] != '*' && text[i] != '/' && !(i > start && (text[i] == '+' || text[i] == '-') && text[i - 1] != 'e'))
                i++;
            const std::string_view token = text.substr(start, i - start);
            if (token.empty()) return false;
            out = Term();
            Length length;
            if (parseNumber(token, out.number) && token.find('%') == std::string_view::npos){
                out.isNumber = true;
                return true;
            }
            if (!parseLength(token, length) || length.isNumber()) return false;
            out.lengths.push_back(length);
            return true;
        }
        bool product(Term& out){
            if (!factor(out)) return false;
            for (;;){
                const bool multiply = eat('*');
                if (!multiply && !eat('/')) return true;
                Term next;
                if (!factor(next)) return false;
                if (multiply && out.isNumber) std::swap(out, next);
                if (!next.isNumber || (!multiply && next.number == 0.0f)) return false;
                scale(out, multiply ? next.number : 1.0f / next.number);
            }
        }
        bool sum(Term& out){
            if (!product(out)) return false;
            for (;;){
                skip();
                // + and - need white space around them in calc()
                if (i + 1 >= text.size() || (text[i] != '+' && text[i] != '-') || text[i + 1] != ' ') return true;
                const float sign = text[i] == '+' ? 1.0f : -1.0f;
                i++;
                Term next;
                if (!product(next) || next.isNumber != out.isNumber) return false;
                add(out, next, sign);
            }
        }
    };

    // a coefficient: one length-percentage, or a linear calc() of them (unit by unit)
    bool parseShapeCoefficient(const std::string& token, std::vector<Length>& out){
        out.clear();
        Length length;
        if (parseLength(token, length)){
            out.push_back(length);
            return true;
        }
        if (token.compare(0, 5, "calc(") != 0) return false;
        ShapeLinearCalc calc{token};
        ShapeLinearCalc::Term term;
        if (!calc.factor(term) || term.isNumber) return false;
        calc.skip();
        if (calc.i != token.size()) return false;
        for (const Length& item : term.lengths)
            if (item.value != 0.0f) out.push_back(item);
        return true;
    }

}

// ---------------------------------------------------------------- BasicShape

struct BasicShape::AnimationComponents {
    using Component = std::vector<Length>;
    std::string name, fillRule;
    std::vector<Component> values;

    static std::shared_ptr<const AnimationComponents> from(const BasicShape& shape){
        if (shape.animationComponents) return shape.animationComponents;
        ShapeAnimationParts parts;
        if (!shapeAnimationParts(shape, parts)) return nullptr;
        auto result = std::make_shared<AnimationComponents>();
        result->name = parts.name;
        result->fillRule = parts.fillRule;
        for (const auto* list : {&parts.values, &parts.radii}){
            for (const auto& token : *list){
                Component component;
                if (!parseShapeCoefficient(token, component)) return nullptr;
                result->values.push_back(std::move(component));
            }
        }
        if (parts.name == "circle" || parts.name == "ellipse"){
            Component x, y;
            std::vector<Rml::Unit> units{Rml::Unit::PERCENT};
            for (const auto& token : parts.position){
                Length length;
                if (parseLength(token, length) && std::find(units.begin(), units.end(), length.unit) == units.end())
                    units.push_back(length.unit);
            }
            // Evaluate the position grammar once per unit with a unit reference box. This
            // keeps edge offsets and relative units as coefficients, without resolving layout.
            for (Rml::Unit unit : units){
                const float base = unit == Rml::Unit::PERCENT ? 1.0f : 0.0f;
                const Resolver probe{nullptr, base, base, unit};
                float cx, cy;
                if (!parsePosition(parts.position, probe, cx, cy, nullptr)) return nullptr;
                const float scale = unit == Rml::Unit::PERCENT ? 100.0f : 1.0f;
                if (cx != 0.0f) x.push_back({cx * scale, unit});
                if (cy != 0.0f) y.push_back({cy * scale, unit});
            }
            result->values.push_back(std::move(x));
            result->values.push_back(std::move(y));
        }
        return result;
    }

    static Component mix(const Component& from, const Component& to, float alpha){
        Component result;
        const auto add = [&](const Component& source, float weight){
            for (const Length& length : source){
                auto found = std::find_if(result.begin(), result.end(), [&](const Length& item){ return item.unit == length.unit; });
                if (found == result.end()) result.push_back({length.value * weight, length.unit});
                else found->value += length.value * weight;
            }
        };
        add(from, 1.0f - alpha);
        add(to, alpha);
        result.erase(std::remove_if(result.begin(), result.end(), [](const Length& length){ return length.value == 0.0f; }), result.end());
        std::sort(result.begin(), result.end(), [](const Length& a, const Length& b){ return a.unit < b.unit; });
        return result;
    }

    static std::string componentText(const Component& value){
        if (value.empty()) return "0px";
        if (value.size() == 1) return Rml::Property(value[0].value, value[0].unit).ToString();
        std::string result = "calc(";
        for (size_t i = 0; i < value.size(); i++){
            const Length& length = value[i];
            result += i == 0 ? (length.value < 0 ? "-" : "") : (length.value < 0 ? " - " : " + ");
            result += Rml::Property(std::fabs(length.value), length.unit).ToString();
        }
        return result + ")";
    }

    std::string serialize(bool used, float width, float height, Rml::Element* element) const{
        const auto value = [&](size_t index, float basis, bool radius = false){
            if (!used) return componentText(values[index]);
            float pixels = 0.0f;
            for (const Length& length : values[index]) pixels += resolve(element, length, basis);
            return num(radius ? std::max(0.0f, pixels) : pixels) + "px";
        };
        std::string result = name + "(";
        if (name == "inset"){
            for (size_t i = 0; i < 4; i++) result += (i ? " " : "") + value(i, i % 2 == 0 ? height : width);
            result += " round";
            for (size_t i = 0; i < 8; i++) result += (i == 4 ? " / " : " ") + value(i + 4, i < 4 ? width : height, true);
        }else if (name == "circle" || name == "ellipse"){
            const size_t radii = name == "circle" ? 1 : 2;
            for (size_t i = 0; i < radii; i++){
                const float basis = radii == 1 ? std::hypot(width, height) / std::sqrt(2.0f) : i == 0 ? width : height;
                result += (i ? " " : "") + value(i, basis, true);
            }
            result += " at " + value(radii, width) + " " + value(radii + 1, height);
        }else{
            result += fillRule + ", ";
            for (size_t i = 0; i < values.size(); i++) result += (i ? i % 2 == 0 ? ", " : " " : "") + value(i, i % 2 == 0 ? width : height);
        }
        return result + ")";
    }
};

Rml::Units BasicShape::unitDependencies() const{
    const auto components = AnimationComponents::from(*this);
    Rml::Units units = Rml::Unit::UNKNOWN;
    if (components){
        for (const auto& component : components->values)
            for (const Length& length : component) units = units | length.unit;
    }else{
        ShapeAnimationParts parts;
        if (!shapeAnimationParts(*this, parts)) return units;
        for (const auto* list : {&parts.values, &parts.position, &parts.radii}){
            for (const auto& token : *list){
                Length length;
                if (parseLength(token, length)) units = units | length.unit;
            }
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

bool BasicShape::interpolate(const BasicShape& to, float alpha, BasicShape& result) const{
    const auto a = AnimationComponents::from(*this), b = AnimationComponents::from(to);
    if (box != to.box || !a || !b || a->name != b->name || a->fillRule != b->fillRule || a->values.size() != b->values.size())
        return false;
    auto mixed = std::make_shared<AnimationComponents>(*a);
    for (size_t i = 0; i < mixed->values.size(); i++) mixed->values[i] = AnimationComponents::mix(a->values[i], b->values[i], alpha);
    result.box = box;
    result.text = mixed->serialize(false, 0, 0, nullptr);
    result.animationComponents = std::move(mixed);
    return true;
}

std::string BasicShape::resolvedText(float width, float height, Rml::Element* element) const{
    return animationComponents ? animationComponents->serialize(true, width, height, element) : text;
}

namespace {

    // The geometry of a basic shape in its reference box (px), what both the SVG and the hit test are made from
    struct ShapeGeometry {
        enum class Kind { RoundedRect, Circle, Ellipse, Polygon, Path };
        Kind kind = Kind::RoundedRect;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;               // RoundedRect
        std::array<std::array<float, 2>, 4> radii{};                 // RoundedRect corners (rx, ry)
        float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;           // Circle (rx), Ellipse
        std::vector<Rml::Vector2f> points;                           // Polygon
        std::string pathData;                                        // Path
        bool evenOdd = false;                                        // Polygon, Path
    };

    bool resolveShapeGeometry(const std::string& text, float width, float height, Rml::Element* element, ShapeGeometry& out,
                              std::string* error){
        if (!(width > 0.0f) || !(height > 0.0f))
            return fail(error, "the reference box is empty");
        const size_t open = text.find('(');
        if (open == std::string::npos || text.empty() || text.back() != ')')
            return fail(error, "'" + text + "' is not a function");
        const std::string name = lower(trim(text.substr(0, open)));
        const std::string arguments = text.substr(open + 1, text.size() - open - 2);
        const Resolver r{element, width, height};
        out = ShapeGeometry();

        std::vector<std::string> tokens = tokenize(arguments);
        if (name == "inset"){
            std::vector<std::string> offsets, radiusTokens;
            splitRound(tokens, offsets, radiusTokens);
            if (offsets.empty() || offsets.size() > 4)
                return fail(error, "inset() needs 1 to 4 offsets");
            std::vector<std::string> o = offsets;
            if (o.size() == 1) o = {o[0], o[0], o[0], o[0]};
            else if (o.size() == 2) o = {o[0], o[1], o[0], o[1]};
            else if (o.size() == 3) o = {o[0], o[1], o[2], o[1]};
            float top, right, bottom, left;
            if (!r.length(o[0], height, top) || !r.length(o[1], width, right) || !r.length(o[2], height, bottom) || !r.length(o[3], width, left))
                return fail(error, "inset() offsets must be lengths or percentages");
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
            out.x = left;
            out.y = top;
            out.w = width - left - right;
            out.h = height - top - bottom;
            if (!radiusTokens.empty() && !parseRadii(radiusTokens, r, out.w, out.h, out.radii, error))
                return false;
            return true;
        }
        if (name == "xywh" || name == "rect"){
            std::vector<std::string> values, radiusTokens;
            splitRound(tokens, values, radiusTokens);
            if (values.size() != 4)
                return fail(error, name + "() needs 4 values");
            if (name == "xywh"){
                if (!r.length(values[0], width, out.x) || !r.length(values[1], height, out.y) || !r.length(values[2], width, out.w) ||
                    !r.length(values[3], height, out.h) || out.w < 0.0f || out.h < 0.0f)
                    return fail(error, "xywh() needs lengths or percentages (non-negative sizes)");
            }else{
                // rect(top right bottom left), offsets from the top and left edges; auto = the edge
                float top = 0.0f, right = width, bottom = height, left = 0.0f;
                const auto edge = [&](const std::string& token, float base, float automatic, float& value){
                    if (lower(token) == "auto"){
                        value = automatic;
                        return true;
                    }
                    return r.length(token, base, value);
                };
                if (!edge(values[0], height, 0.0f, top) || !edge(values[1], width, width, right) || !edge(values[2], height, height, bottom) ||
                    !edge(values[3], width, 0.0f, left))
                    return fail(error, "rect() needs lengths, percentages or auto");
                out.x = left;
                out.y = top;
                out.w = std::max(0.0f, right - left);
                out.h = std::max(0.0f, bottom - top);
            }
            if (!radiusTokens.empty() && !parseRadii(radiusTokens, r, out.w, out.h, out.radii, error))
                return false;
            return true;
        }
        if (name == "circle" || name == "ellipse"){
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
                return false;
            const bool circle = name == "circle";
            if (radiusTokens.size() > (circle ? 1u : 2u))
                return fail(error, name + "() has too many radii");
            const auto sideRadius = [&](const std::string& token, bool horizontal, float& value) -> bool {
                const std::string keyword = lower(token);
                const float dx[2] = {std::fabs(cx), std::fabs(width - cx)};
                const float dy[2] = {std::fabs(cy), std::fabs(height - cy)};
                if (keyword == "closest-side" || keyword == "farthest-side"){
                    const bool closest = keyword == "closest-side";
                    if (circle){
                        const float values[4] = {dx[0], dx[1], dy[0], dy[1]};
                        value = closest ? *std::min_element(values, values + 4) : *std::max_element(values, values + 4);
                    }else{
                        const float* d = horizontal ? dx : dy;
                        value = closest ? std::min(d[0], d[1]) : std::max(d[0], d[1]);
                    }
                    return true;
                }
                const float base = circle ? std::sqrt(width * width + height * height) / std::sqrt(2.0f) : (horizontal ? width : height);
                return r.length(token, base, value) && value >= 0.0f;
            };
            out.cx = cx;
            out.cy = cy;
            if (circle){
                out.kind = ShapeGeometry::Kind::Circle;
                if (!sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[0], true, out.rx))
                    return fail(error, "circle() radius must be a length, percentage, closest-side or farthest-side");
                out.ry = out.rx;
                return true;
            }
            out.kind = ShapeGeometry::Kind::Ellipse;
            if (radiusTokens.size() == 1)
                return fail(error, "ellipse() needs two radii or none");
            if (!sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[0], true, out.rx) ||
                !sideRadius(radiusTokens.empty() ? "closest-side" : radiusTokens[1], false, out.ry))
                return fail(error, "ellipse() radii must be lengths, percentages, closest-side or farthest-side");
            return true;
        }
        if (name == "polygon" || name == "path"){
            // an optional fill rule before the first comma
            size_t start = 0;
            if (!tokens.empty() && (lower(tokens[0]) == "nonzero" || lower(tokens[0]) == "evenodd")){
                out.evenOdd = lower(tokens[0]) == "evenodd";
                if (tokens.size() < 2 || tokens[1] != ",")
                    return fail(error, name + "() needs a comma after the fill rule");
                start = 2;
            }
            if (name == "path"){
                out.kind = ShapeGeometry::Kind::Path;
                if (tokens.size() != start + 1)
                    return fail(error, "path() needs one string of path data");
                out.pathData = unquote(tokens[start]);
                if (out.pathData.empty())
                    return fail(error, "path() has no path data");
                return true;
            }
            out.kind = ShapeGeometry::Kind::Polygon;
            std::vector<std::string> pair;
            const auto flush = [&]() -> bool {
                if (pair.size() != 2)
                    return false;
                float x, y;
                if (!r.length(pair[0], width, x) || !r.length(pair[1], height, y))
                    return false;
                out.points.emplace_back(x, y);
                pair.clear();
                return true;
            };
            for (size_t i = start; i < tokens.size(); i++){
                if (tokens[i] == ","){
                    if (!flush())
                        return fail(error, "polygon() points must be pairs of lengths or percentages");
                }else{
                    pair.push_back(tokens[i]);
                }
            }
            if (!flush() || out.points.size() < 3)
                return fail(error, "polygon() needs at least 3 points of lengths or percentages");
            return true;
        }
        return fail(error, "'" + name + "()' is not a basic shape (inset, circle, ellipse, polygon, path, xywh, rect)");
    }

    // ---- SVG path data (path()) as polylines, for the hit test

    struct ShapePathReader {
        const std::string& d;
        size_t i = 0;

        void skip(){
            while (i < d.size() && (std::isspace((unsigned char)d[i]) || d[i] == ','))
                i++;
        }
        bool number(float& value){
            skip();
            const size_t begin = i;
            if (i < d.size() && (d[i] == '+' || d[i] == '-'))
                i++;
            bool digits = false;
            while (i < d.size() && std::isdigit((unsigned char)d[i])){
                i++;
                digits = true;
            }
            if (i < d.size() && d[i] == '.'){
                i++;
                while (i < d.size() && std::isdigit((unsigned char)d[i])){
                    i++;
                    digits = true;
                }
            }
            if (!digits){
                i = begin;
                return false;
            }
            if (i < d.size() && (d[i] == 'e' || d[i] == 'E')){
                size_t e = i + 1;
                if (e < d.size() && (d[e] == '+' || d[e] == '-'))
                    e++;
                if (e < d.size() && std::isdigit((unsigned char)d[e])){
                    i = e;
                    while (i < d.size() && std::isdigit((unsigned char)d[i]))
                        i++;
                }
            }
            return parseNumber(std::string_view(d).substr(begin, i - begin), value);
        }
        // an arc flag: one 0 or 1, separators optional around it
        bool flag(bool& value){
            skip();
            if (i >= d.size() || (d[i] != '0' && d[i] != '1'))
                return false;
            value = d[i++] == '1';
            return true;
        }
        bool numbers(float* values, int count){
            for (int k = 0; k < count; k++){
                if (!number(values[k]))
                    return false;
            }
            return true;
        }
    };

    int curveSegments(float length){
        return std::max(4, std::min(64, (int)std::ceil(length / 2.0f)));
    }

    // The subpaths of SVG path data, curves flattened; on an error, what came before it (SVG 2, 9.5.4)
    void flattenShapePath(const std::string& d, std::vector<std::vector<Rml::Vector2f>>& subpaths){
        using V = Rml::Vector2f;
        ShapePathReader reader{d};
        V current(0, 0), start(0, 0), lastControl(0, 0);
        char previous = 0;
        char command = 0;
        bool newSubpath = true;
        const auto lineTo = [&](V p){
            if (newSubpath){
                subpaths.emplace_back();
                subpaths.back().push_back(current);
                newSubpath = false;
            }
            subpaths.back().push_back(p);
            current = p;
        };
        const auto cubic = [&](V c1, V c2, V p){
            const V p0 = current;
            const int n = curveSegments((c1 - p0).Magnitude() + (c2 - c1).Magnitude() + (p - c2).Magnitude());
            for (int k = 1; k <= n; k++){
                const float t = (float)k / (float)n, u = 1.0f - t;
                lineTo(p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + p * (t * t * t));
            }
        };
        const auto quad = [&](V c, V p){
            const V p0 = current;
            const int n = curveSegments((c - p0).Magnitude() + (p - c).Magnitude());
            for (int k = 1; k <= n; k++){
                const float t = (float)k / (float)n, u = 1.0f - t;
                lineTo(p0 * (u * u) + c * (2.0f * u * t) + p * (t * t));
            }
        };
        // SVG 2 Appendix B.2.4: endpoint to centre parameterization
        const auto arc = [&](float rx, float ry, float rotation, bool large, bool sweep, V p){
            const V p0 = current;
            rx = std::fabs(rx);
            ry = std::fabs(ry);
            if (rx == 0.0f || ry == 0.0f || p0 == p){
                lineTo(p);
                return;
            }
            const float phi = rotation * 3.14159265358979f / 180.0f;
            const float c = std::cos(phi), s = std::sin(phi);
            const float dx = (p0.x - p.x) * 0.5f, dy = (p0.y - p.y) * 0.5f;
            const float x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
            const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
            if (lambda > 1.0f){
                rx *= std::sqrt(lambda);
                ry *= std::sqrt(lambda);
            }
            const float numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
            const float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
            float coefficient = denominator > 0.0f ? std::sqrt(std::max(0.0f, numerator / denominator)) : 0.0f;
            if (large == sweep)
                coefficient = -coefficient;
            const float cxp = coefficient * rx * y1 / ry, cyp = -coefficient * ry * x1 / rx;
            const float cx = c * cxp - s * cyp + (p0.x + p.x) * 0.5f, cy = s * cxp + c * cyp + (p0.y + p.y) * 0.5f;
            const auto angle = [](float ux, float uy, float vx, float vy){ return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
            const float ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry, vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
            const float theta = angle(1.0f, 0.0f, ux, uy);
            float delta = angle(ux, uy, vx, vy);
            const float tau = 2.0f * 3.14159265358979f;
            if (!sweep && delta > 0.0f)
                delta -= tau;
            else if (sweep && delta < 0.0f)
                delta += tau;
            const int n = std::max(4, std::min(128, (int)std::ceil(std::fabs(delta) * std::max(rx, ry) / 2.0f)));
            for (int k = 1; k < n; k++){
                const float a = theta + delta * (float)k / (float)n;
                const float ex = rx * std::cos(a), ey = ry * std::sin(a);
                lineTo(V(cx + c * ex - s * ey, cy + s * ex + c * ey));
            }
            lineTo(p);
        };

        while (true){
            reader.skip();
            if (reader.i >= d.size())
                return;
            const char next = d[reader.i];
            if (std::isalpha((unsigned char)next)){
                command = next;
                reader.i++;
            }else if (command == 0 || command == 'z' || command == 'Z'){
                return;
            }
            const bool relative = std::islower((unsigned char)command) != 0;
            const V base = relative ? current : V(0, 0);
            float v[7];
            switch (command){
                case 'M': case 'm':
                    if (!reader.numbers(v, 2))
                        return;
                    current = base + V(v[0], v[1]);
                    start = current;
                    newSubpath = true;
                    // following pairs are line-tos
                    command = relative ? 'l' : 'L';
                    break;
                case 'L': case 'l':
                    if (!reader.numbers(v, 2))
                        return;
                    lineTo(base + V(v[0], v[1]));
                    break;
                case 'H': case 'h':
                    if (!reader.numbers(v, 1))
                        return;
                    lineTo(V(relative ? current.x + v[0] : v[0], current.y));
                    break;
                case 'V': case 'v':
                    if (!reader.numbers(v, 1))
                        return;
                    lineTo(V(current.x, relative ? current.y + v[0] : v[0]));
                    break;
                case 'C': case 'c':{
                    if (!reader.numbers(v, 6))
                        return;
                    const V c2 = base + V(v[2], v[3]);
                    cubic(base + V(v[0], v[1]), c2, base + V(v[4], v[5]));
                    lastControl = c2;
                    break;
                }
                case 'S': case 's':{
                    if (!reader.numbers(v, 4))
                        return;
                    const bool smooth = previous == 'C' || previous == 'c' || previous == 'S' || previous == 's';
                    const V c1 = smooth ? current * 2.0f - lastControl : current;
                    const V c2 = base + V(v[0], v[1]);
                    cubic(c1, c2, base + V(v[2], v[3]));
                    lastControl = c2;
                    break;
                }
                case 'Q': case 'q':{
                    if (!reader.numbers(v, 4))
                        return;
                    const V c = base + V(v[0], v[1]);
                    quad(c, base + V(v[2], v[3]));
                    lastControl = c;
                    break;
                }
                case 'T': case 't':{
                    if (!reader.numbers(v, 2))
                        return;
                    const bool smooth = previous == 'Q' || previous == 'q' || previous == 'T' || previous == 't';
                    const V c = smooth ? current * 2.0f - lastControl : current;
                    quad(c, base + V(v[0], v[1]));
                    lastControl = c;
                    break;
                }
                case 'A': case 'a':{
                    bool large = false, sweep = false;
                    if (!reader.numbers(v, 3) || !reader.flag(large) || !reader.flag(sweep) || !reader.numbers(v + 3, 2))
                        return;
                    arc(v[0], v[1], v[2], large, sweep, base + V(v[3], v[4]));
                    break;
                }
                case 'Z': case 'z':
                    current = start;
                    newSubpath = true;
                    break;
                default:
                    return;
            }
            previous = command;
        }
    }

    // The winding number of the closed polylines around a point (each implicitly closed)
    int shapeWinding(const std::vector<Rml::Vector2f>& polygon, Rml::Vector2f p){
        int winding = 0;
        const size_t n = polygon.size();
        for (size_t k = 0; k < n; k++){
            const Rml::Vector2f a = polygon[k], b = polygon[(k + 1) % n];
            const float side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
            if (a.y <= p.y){
                if (b.y > p.y && side > 0.0f)
                    winding++;
            }else if (b.y <= p.y && side < 0.0f){
                winding--;
            }
        }
        return winding;
    }

    bool insideRoundedRect(const ShapeGeometry& g, Rml::Vector2f p){
        if (p.x < g.x || p.y < g.y || p.x > g.x + g.w || p.y > g.y + g.h)
            return false;
        // the distance into the rectangle from the corner's two edges: top-left, top-right, bottom-right, bottom-left
        const Rml::Vector2f d[4] = {{p.x - g.x, p.y - g.y}, {g.x + g.w - p.x, p.y - g.y}, {g.x + g.w - p.x, g.y + g.h - p.y},
                                    {p.x - g.x, g.y + g.h - p.y}};
        for (int corner = 0; corner < 4; corner++){
            const float rx = g.radii[corner][0], ry = g.radii[corner][1];
            if (rx > 0.0f && ry > 0.0f && d[corner].x < rx && d[corner].y < ry){
                const float qx = (rx - d[corner].x) / rx, qy = (ry - d[corner].y) / ry;
                if (qx * qx + qy * qy > 1.0f)
                    return false;
            }
        }
        return true;
    }

}

std::string BasicShape::toSvg(float width, float height, Rml::Element* element, std::string* error) const{
    ShapeGeometry g;
    if (!resolveShapeGeometry(resolvedText(width, height, element), width, height, element, g, error))
        return std::string();
    std::string shapeMarkup;
    switch (g.kind){
        case ShapeGeometry::Kind::RoundedRect:
            shapeMarkup = "<path d=\"" + roundedRectPath(g.x, g.y, g.w, g.h, g.radii) + "\"";
            break;
        case ShapeGeometry::Kind::Circle:
            shapeMarkup = "<circle cx=\"" + num(g.cx) + "\" cy=\"" + num(g.cy) + "\" r=\"" + num(g.rx) + "\"";
            break;
        case ShapeGeometry::Kind::Ellipse:
            shapeMarkup = "<ellipse cx=\"" + num(g.cx) + "\" cy=\"" + num(g.cy) + "\" rx=\"" + num(g.rx) + "\" ry=\"" + num(g.ry) + "\"";
            break;
        case ShapeGeometry::Kind::Polygon:{
            std::string points;
            for (const Rml::Vector2f& p : g.points)
                points += (points.empty() ? "" : " ") + num(p.x) + "," + num(p.y);
            shapeMarkup = "<polygon points=\"" + points + "\"";
            break;
        }
        case ShapeGeometry::Kind::Path:{
            std::string escaped;
            for (char c : g.pathData)
                escaped += c == '"' ? std::string("&quot;") : c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : std::string(1, c);
            shapeMarkup = "<path d=\"" + escaped + "\"";
            break;
        }
    }
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(width) + "\" height=\"" + num(height) + "\" viewBox=\"0 0 " +
           num(width) + " " + num(height) + "\">" + shapeMarkup + " fill=\"#ffffff\" fill-rule=\"" + (g.evenOdd ? "evenodd" : "nonzero") +
           "\"/></svg>";
}

bool BasicShape::contains(float width, float height, Rml::Element* element, Rml::Vector2f point) const{
    ShapeGeometry g;
    if (!resolveShapeGeometry(resolvedText(width, height, element), width, height, element, g, nullptr))
        return false;
    switch (g.kind){
        case ShapeGeometry::Kind::RoundedRect: return insideRoundedRect(g, point);
        case ShapeGeometry::Kind::Circle:
        case ShapeGeometry::Kind::Ellipse:{
            if (!(g.rx > 0.0f) || !(g.ry > 0.0f))
                return false;
            const float dx = (point.x - g.cx) / g.rx, dy = (point.y - g.cy) / g.ry;
            return dx * dx + dy * dy <= 1.0f;
        }
        case ShapeGeometry::Kind::Polygon:{
            const int winding = shapeWinding(g.points, point);
            return g.evenOdd ? (winding & 1) != 0 : winding != 0;
        }
        case ShapeGeometry::Kind::Path:{
            std::vector<std::vector<Rml::Vector2f>> subpaths;
            flattenShapePath(g.pathData, subpaths);
            int winding = 0;
            for (const std::vector<Rml::Vector2f>& subpath : subpaths)
                winding += shapeWinding(subpath, point);
            return g.evenOdd ? (winding & 1) != 0 : winding != 0;
        }
    }
    return false;
}

// S92: the analytic clips (R8), from the same geometry as toSvg and contains (S93)
bool BasicShape::toRoundedRect(float width, float height, Rml::Element* element, Rml::Rectanglef& rect, Rml::Array<Rml::Vector2f, 4>& radii) const{
    ShapeGeometry g;
    if (!resolveShapeGeometry(resolvedText(width, height, element), width, height, element, g, nullptr))
        return false;
    switch (g.kind){
        case ShapeGeometry::Kind::RoundedRect:
            rect = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(g.x, g.y), Rml::Vector2f(std::max(g.w, 0.0f), std::max(g.h, 0.0f)));
            for (size_t corner = 0; corner < 4; corner++)
                radii[corner] = Rml::Vector2f(g.radii[corner][0], g.radii[corner][1]);
            return true;
        case ShapeGeometry::Kind::Circle:
        case ShapeGeometry::Kind::Ellipse:{
            const float rx = std::max(g.rx, 0.0f), ry = std::max(g.ry, 0.0f);
            rect = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(g.cx - rx, g.cy - ry), Rml::Vector2f(2.0f * rx, 2.0f * ry));
            for (size_t corner = 0; corner < 4; corner++)
                radii[corner] = Rml::Vector2f(rx, ry);
            return true;
        }
        default:
            return false;
    }
}

bool BasicShape::toConvexPolygon(float width, float height, Rml::Element* element, std::vector<Rml::Vector2f>& points) const{
    points.clear();
    ShapeGeometry g;
    if (!resolveShapeGeometry(resolvedText(width, height, element), width, height, element, g, nullptr) || g.kind != ShapeGeometry::Kind::Polygon)
        return false;
    // (the fill rule changes nothing for a convex polygon) a point equal to the one before it adds no edge
    for (const Rml::Vector2f& point : g.points){
        if (points.empty() || points.back() != point)
            points.push_back(point);
    }
    if (points.size() > 1 && points.front() == points.back())
        points.pop_back();
    const size_t n = points.size();
    if (n < 3 || n > (size_t)Rml::ClipShape::MaxPolygonPoints){
        points.clear();
        return false;
    }
    // convex and simple: every turn the same way, and once around (a star turns twice)
    double area = 0.0, turning = 0.0;
    int sign = 0;
    for (size_t i = 0; i < n; i++){
        const Rml::Vector2f a = points[i], b = points[(i + 1) % n], c = points[(i + 2) % n];
        area += (double)a.x * b.y - (double)b.x * a.y;
        const double ex = b.x - a.x, ey = b.y - a.y, fx = c.x - b.x, fy = c.y - b.y;
        const double cross = ex * fy - ey * fx;
        if (std::abs(cross) > 1e-9){
            const int turn = cross > 0.0 ? 1 : -1;
            if (sign != 0 && turn != sign){
                points.clear();
                return false;
            }
            sign = turn;
        }
        turning += std::atan2(cross, ex * fx + ey * fy);
    }
    if (sign == 0 || std::abs(std::abs(turning) - 2.0 * 3.14159265358979323846) > 1e-3 || std::abs(area) < 1e-6){
        points.clear();
        return false;
    }
    if (area < 0.0)
        std::reverse(points.begin(), points.end());
    return true;
}

// ---------------------------------------------------------------- hit test

namespace {

    // the shapes of html-shape("...") items in a mask-image list's text, parsed once per shape text
    const BasicShape* cachedHitShape(const std::string& text){
        static std::mutex mutex;
        static std::unordered_map<std::string, std::unique_ptr<BasicShape>> shapes;
        std::lock_guard<std::mutex> lock(mutex);
        auto found = shapes.find(text);
        if (found != shapes.end())
            return found->second.get();
        std::unique_ptr<BasicShape> shape = std::make_unique<BasicShape>();
        if (!BasicShape::parse(text, *shape, nullptr))
            shape.reset();
        // bounded (a document has a handful of clip-paths; animated ones make new texts): the main thread is the only
        // caller, so no pointer handed out earlier is still in use
        if (shapes.size() >= 256)
            shapes.clear();
        return shapes.emplace(text, std::move(shape)).first->second.get();
    }

    // the quoted argument of html-shape( at `at` (serializeString: \-escapes); false when malformed
    bool shapeArgument(const std::string& text, size_t at, std::string& out){
        size_t i = at;
        while (i < text.size() && std::isspace((unsigned char)text[i]))
            i++;
        if (i >= text.size() || (text[i] != '"' && text[i] != '\''))
            return false;
        const char quote = text[i++];
        out.clear();
        for (; i < text.size(); i++){
            if (text[i] == '\\' && i + 1 < text.size()){
                out += text[++i];
                continue;
            }
            if (text[i] == quote)
                return true;
            out += text[i];
        }
        return false;
    }

}

bool jasperengine::htmlui::compat::clipPathHitTest(Rml::Element* element, Rml::Vector2f point){
    const Rml::Property* property = element->GetProperty(Rml::PropertyId::MaskImage);
    if (!property)
        return true;
    const std::string list = property->ToString();
    static const std::string function = "html-shape(";
    std::string text;
    for (size_t at = list.find(function); at != std::string::npos; at = list.find(function, at + function.size())){
        if (!shapeArgument(list, at + function.size(), text))
            continue;
        const BasicShape* shape = cachedHitShape(text);
        if (!shape)
            continue;
        const Rml::Rectanglef box = boxRectangle(element, shape->box);
        const Rml::Vector2f local = point - element->GetAbsoluteOffset(Rml::BoxArea::Border) - box.TopLeft();
        if (!shape->contains(box.Width(), box.Height(), element, local))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- decorator

struct HtmlShapeDecorator::Data {
    Rml::Geometry geometry;
    Rml::CallbackTexture texture;
    std::string svg;
    Rml::Vector2i pixels{0, 0};
    Rml::Vector2f offset{0, 0};
    // S92 R8: a rounded shape of the border box as a clip (relative to the border box), with its mesh for the stencil
    bool clips = false;
    Rml::ClipShape clipShape;
    Rml::Geometry clipGeometry;
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

    // the border box's inset(), circle(), ellipse(), xywh() and rect(): a clip (S92 R8), unless the element's opacity
    // fades the mask (only an opaque mask is a clip)
    Rml::Rectanglef rect;
    Rml::Array<Rml::Vector2f, 4> radii;
    std::vector<Rml::Vector2f> polygon;
    if (shape.box == Rml::BoxArea::Border && element->GetDrawOpacity() >= 1.0f && shape.toConvexPolygon(box.Width(), box.Height(), element, polygon)){
        // S92: a convex polygon() too, its bounds the rectangle; a fan of it for the stencil
        Rml::Vector2f lo = polygon[0], hi = polygon[0];
        data->clipShape.polygon_points = (int)polygon.size();
        Rml::Mesh clipMesh;
        for (size_t i = 0; i < polygon.size(); i++){
            const Rml::Vector2f point = polygon[i] + box.TopLeft();
            data->clipShape.polygon[i] = point;
            lo = Rml::Math::Min(lo, polygon[i]);
            hi = Rml::Math::Max(hi, polygon[i]);
            Rml::Vertex vertex;
            vertex.position = point;
            vertex.colour = Rml::ColourbPremultiplied(255);
            clipMesh.vertices.push_back(vertex);
            if (i >= 2){
                clipMesh.indices.push_back(0);
                clipMesh.indices.push_back((int)i - 1);
                clipMesh.indices.push_back((int)i);
            }
        }
        data->clipShape.rect = Rml::Rectanglef::FromCorners(lo, hi).Translate(box.TopLeft());
        data->clipGeometry = renderManager->MakeGeometry(std::move(clipMesh));
        data->clips = (bool)data->clipGeometry;
    }else if (shape.box == Rml::BoxArea::Border && element->GetDrawOpacity() >= 1.0f && shape.toRoundedRect(box.Width(), box.Height(), element, rect, radii) &&
        rect.Width() > 0.0f && rect.Height() > 0.0f){
        data->clipShape.rect = rect.Translate(box.TopLeft());
        Rml::CornerSizes2 corners;
        for (size_t corner = 0; corner < 4; corner++){
            const bool curved = radii[corner].x > 0.0f && radii[corner].y > 0.0f;
            data->clipShape.radii[corner] = curved ? radii[corner] : Rml::Vector2f(0.0f);
            corners[corner] = data->clipShape.radii[corner];
        }
        Rml::Mesh clipMesh;
        Rml::MeshUtilities::GenerateBackground(clipMesh, Rml::RenderBox(data->clipShape.rect.Size(), data->clipShape.rect.TopLeft(), Rml::EdgeSizes{},
                                                                        corners), Rml::ColourbPremultiplied(255));
        data->clipGeometry = renderManager->MakeGeometry(std::move(clipMesh));
        data->clips = (bool)data->clipGeometry;
    }
    return reinterpret_cast<Rml::DecoratorDataHandle>(data);
}

bool HtmlShapeDecorator::GetMaskClipShape(Rml::Element* /*element*/, Rml::DecoratorDataHandle element_data, Rml::ClipShape& clipShape,
                                          Rml::Geometry*& geometry) const{
    Data* data = reinterpret_cast<Data*>(element_data);
    if (!data || !data->clips)
        return false;
    clipShape = data->clipShape;
    geometry = &data->clipGeometry;
    return true;
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
