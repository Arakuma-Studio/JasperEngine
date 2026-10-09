// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/MediaQuery.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

using namespace jasperengine::htmlui::compat;

namespace {

    // the gap RmlUi's inclusive min-/max- features need for CSS's strict < and > (spec 3.9.4)
    constexpr double kLengthStep = 0.02;
    constexpr double kResolutionStep = 0.001;
    constexpr double kRatioStep = 0.0001;
    constexpr size_t kMaxBlocks = 64;

    using Dnf = std::vector<MediaConjunction>;

    Dnf always(){ return Dnf{MediaConjunction{}}; }
    Dnf never(){ return Dnf{}; }

    Dnf conjoinDnf(const Dnf& a, const Dnf& b){
        Dnf result;
        for (const MediaConjunction& x : a){
            for (const MediaConjunction& y : b){
                MediaConjunction c = MediaConjunction::conjoin(x, y);
                if (!c.contradiction)
                    result.push_back(std::move(c));
                if (result.size() > kMaxBlocks * 4)
                    return result;
            }
        }
        return result;
    }

    Dnf disjoinDnf(Dnf a, const Dnf& b){
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    // the simplest fraction within 1e-7 of ratio (16/9 stays 16/9), by continued fractions
    std::string ratioText(double ratio){
        if (!(ratio > 0))
            return "0/1";
        long long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
        double x = ratio;
        for (int i = 0; i < 32; i++){
            const long long a = (long long)std::floor(x);
            const long long h2 = a * h1 + h0, k2 = a * k1 + k0;
            h0 = h1; h1 = h2; k0 = k1; k1 = k2;
            if (k1 > 0 && std::fabs((double)h1 / (double)k1 - ratio) < 1e-7 * ratio)
                break;
            const double fraction = x - (double)a;
            if (fraction < 1e-12 || k1 > 1000000)
                break;
            x = 1.0 / fraction;
        }
        if (k1 <= 0)
            return "0/1";
        return std::to_string(h1) + "/" + std::to_string(k1);
    }

    // a parsed media feature: its positive and negated DNF
    struct Atom {
        Dnf positive, negative;
    };

    Atom staticAtom(bool value){
        return value ? Atom{always(), never()} : Atom{never(), always()};
    }

    Atom rangeAtom(MediaConjunction::Range MediaConjunction::* member, std::optional<double> min, std::optional<double> max,
                   double step){
        Atom atom;
        MediaConjunction positive;
        (positive.*member).min = min;
        (positive.*member).max = max;
        atom.positive = {positive};
        if (min){
            MediaConjunction below;
            (below.*member).max = *min - step;
            atom.negative.push_back(below);
        }
        if (max){
            MediaConjunction above;
            (above.*member).min = *max + step;
            atom.negative.push_back(above);
        }
        return atom;
    }

    class Normalizer {
    public:
        Normalizer(const CompatOptions& options, DiagnosticSink* sink, SourceRef ref): options(options), sink(sink), ref(ref) {}

        // one <media-query>; false when invalid (never matches)
        bool query(const std::vector<ComponentValue>& values, Dnf& out){
            std::vector<const ComponentValue*> items;
            for (const ComponentValue& v : values){
                if (!v.isWhitespace() && !v.isToken(TokenType::Comment))
                    items.push_back(&v);
            }
            if (items.empty()){
                out = never();
                return false;
            }
            size_t i = 0;
            // [not | only]? <media-type> [and <condition-without-or>]?
            if (items[0]->isToken(TokenType::Ident)){
                const std::string first = lowerAscii(items[0]->token.value);
                bool negated = false;
                if ((first == "not" || first == "only") && items.size() >= 2 && items[1]->isToken(TokenType::Ident)){
                    negated = first == "not";
                    i = 1;
                    rewritten = true;
                }else if (first == "not"){
                    // "not (condition)"
                    return condition(items, 0, items.size(), out);
                }
                const std::string type = lowerAscii(items[i]->token.value);
                if (type == "and" || type == "or" || type == "not" || type == "only")
                    return invalid(out);
                bool matches = mediaType(type);
                if (negated)
                    matches = !matches;
                rewritten = true;
                i++;
                Dnf result = matches ? always() : never();
                if (i < items.size()){
                    if (!items[i]->isToken(TokenType::Ident) || !equalsNoCase(items[i]->token.value, "and") || i + 1 >= items.size())
                        return invalid(out);
                    Dnf rest;
                    if (!condition(items, i + 1, items.size(), rest, false))
                        return invalid(out);
                    if (negated){
                        // "not print and (x)" negates the whole query
                        Dnf positive = conjoinDnf(mediaType(type) ? always() : never(), rest);
                        result = negate(positive);
                    }else{
                        result = conjoinDnf(result, rest);
                    }
                }
                out = result;
                return true;
            }
            return condition(items, 0, items.size(), out);
        }

        bool rewritten = false;   // the RCSS is not the CSS text as written

    private:
        static bool mediaType(const std::string& type){
            return type == "all" || type == "screen";
        }

        bool invalid(Dnf& out){
            out = never();
            report(codes::MediaFeatureUnsupported, "invalid media query; it never matches", "check the media query syntax",
                   "");
            return false;
        }

        // <media-condition> over items[begin, end)
        bool condition(const std::vector<const ComponentValue*>& items, size_t begin, size_t end, Dnf& out, bool allowOr = true){
            if (begin >= end)
                return false;
            if (items[begin]->isToken(TokenType::Ident) && equalsNoCase(items[begin]->token.value, "not")){
                if (end - begin != 2)
                    return false;
                Dnf inner;
                if (!inParens(*items[begin + 1], inner))
                    return false;
                rewritten = true;
                out = negate(inner);
                return true;
            }
            Dnf result;
            if (!inParens(*items[begin], result))
                return false;
            std::string joiner;
            for (size_t i = begin + 1; i < end; i += 2){
                if (!items[i]->isToken(TokenType::Ident) || i + 1 >= end)
                    return false;
                const std::string word = lowerAscii(items[i]->token.value);
                if (word != "and" && word != "or")
                    return false;
                if (!joiner.empty() && joiner != word)
                    return false;   // and/or cannot be mixed without parentheses
                if (word == "or" && !allowOr)
                    return false;
                joiner = word;
                Dnf next;
                if (!inParens(*items[i + 1], next))
                    return false;
                if (word == "and"){
                    result = conjoinDnf(result, next);
                }else{
                    rewritten = true;
                    result = disjoinDnf(std::move(result), next);
                }
            }
            out = result;
            return true;
        }

        Dnf negate(const Dnf& dnf){
            // not (A or B ...) = not A and not B; not (a and b) = not a or not b
            Dnf result = always();
            for (const MediaConjunction& conjunction : dnf){
                result = conjoinDnf(result, negateConjunction(conjunction));
                if (result.size() > kMaxBlocks * 4)
                    break;
            }
            return result;
        }

        static Dnf negateRange(MediaConjunction::Range MediaConjunction::* member, const MediaConjunction::Range& range, double step){
            Dnf result;
            if (range.min){
                MediaConjunction c;
                (c.*member).max = *range.min - step;
                result.push_back(c);
            }
            if (range.max){
                MediaConjunction c;
                (c.*member).min = *range.max + step;
                result.push_back(c);
            }
            return result;
        }

        Dnf negateConjunction(const MediaConjunction& c){
            if (c.contradiction)
                return always();
            Dnf result;
            auto add = [&](const Dnf& part){ result.insert(result.end(), part.begin(), part.end()); };
            add(negateRange(&MediaConjunction::width, c.width, kLengthStep));
            add(negateRange(&MediaConjunction::height, c.height, kLengthStep));
            add(negateRange(&MediaConjunction::resolution, c.resolution, kResolutionStep));
            add(negateRange(&MediaConjunction::aspect, c.aspect, kRatioStep));
            if (!c.orientation.empty()){
                MediaConjunction o;
                o.orientation = c.orientation == "landscape" ? "portrait" : "landscape";
                result.push_back(o);
            }
            if (!c.theme.empty()){
                MediaConjunction t;
                t.negatedTheme = c.theme;
                result.push_back(t);
            }
            if (!c.negatedTheme.empty()){
                MediaConjunction t;
                t.theme = c.negatedTheme;
                result.push_back(t);
            }
            if (c.inexpressible){
                MediaConjunction x;
                x.inexpressible = true;
                result.push_back(x);
            }
            return result;   // empty: the negation of "always" never matches
        }

        // ( <media-condition> ) | <media-feature> | <general-enclosed>
        bool inParens(const ComponentValue& value, Dnf& out){
            if (!value.isBlock(TokenType::LeftParen)){
                if (value.kind == ComponentValue::Kind::Function){
                    // <general-enclosed>: valid syntax, unknown meaning
                    unknown(serialize(value));
                    out = never();
                    return true;
                }
                return false;
            }
            std::vector<const ComponentValue*> inner;
            for (const ComponentValue& v : value.children){
                if (!v.isWhitespace() && !v.isToken(TokenType::Comment))
                    inner.push_back(&v);
            }
            if (inner.empty())
                return false;
            if (inner[0]->isBlock(TokenType::LeftParen) ||
                (inner[0]->isToken(TokenType::Ident) && equalsNoCase(inner[0]->token.value, "not") && inner.size() == 2)){
                return condition(inner, 0, inner.size(), out);
            }
            Atom atom;
            if (!feature(value, inner, atom))
                return false;
            out = atom.positive;
            return true;
        }

        // value of a feature: a length in px, a resolution in x, a ratio, a number, an ident
        struct Value {
            enum class Kind { Length, Resolution, Ratio, Number, Ident, Invalid } kind = Kind::Invalid;
            double number = 0;
            std::string ident;
        };

        Value readValue(const std::vector<const ComponentValue*>& tokens){
            Value v;
            if (tokens.size() == 3 && tokens[0]->isToken(TokenType::Number) && tokens[1]->isToken(TokenType::Delim) &&
                tokens[1]->token.isDelim('/') && tokens[2]->isToken(TokenType::Number)){
                if (tokens[2]->token.number == 0)
                    return v;
                v.kind = Value::Kind::Ratio;
                v.number = tokens[0]->token.number / tokens[2]->token.number;
                return v;
            }
            if (tokens.size() != 1)
                return v;
            const ComponentValue& t = *tokens[0];
            if (t.isToken(TokenType::Number)){
                v.kind = Value::Kind::Number;
                v.number = t.token.number;
                return v;
            }
            if (t.isToken(TokenType::Ident)){
                v.kind = Value::Kind::Ident;
                v.ident = lowerAscii(t.token.value);
                return v;
            }
            if (t.isToken(TokenType::Dimension)){
                const std::string unit = lowerAscii(t.token.unit);
                const UnitCategory category = unitCategory(unit);
                if (unit == "em" || unit == "rem"){
                    v.kind = Value::Kind::Length;
                    v.number = t.token.number * 16.0;
                    emConverted = true;
                    return v;
                }
                if (category == UnitCategory::AbsoluteLength){
                    v.kind = Value::Kind::Length;
                    v.number = t.token.number * canonicalFactor(unit).value_or(1.0);
                    if (unit != "px")
                        emConverted = true;
                    return v;
                }
                if (category == UnitCategory::Resolution){
                    v.kind = Value::Kind::Resolution;
                    v.number = t.token.number * canonicalFactor(unit).value_or(1.0);
                    if (unit != "x" && unit != "dppx")
                        emConverted = true;
                    return v;
                }
            }
            return v;
        }

        bool isComparison(const std::vector<const ComponentValue*>& tokens, size_t i, std::string& op, size_t& length){
            if (i >= tokens.size() || !tokens[i]->isToken(TokenType::Delim))
                return false;
            const std::string c = tokens[i]->token.value;
            if (c != "<" && c != ">" && c != "=")
                return false;
            op = c;
            length = 1;
            if (c != "=" && i + 1 < tokens.size() && tokens[i + 1]->isToken(TokenType::Delim) && tokens[i + 1]->token.value == "=" &&
                tokens[i]->offset + tokens[i]->length == tokens[i + 1]->offset){
                op += "=";
                length = 2;
            }
            return true;
        }

        // <mf-plain> | <mf-boolean> | <mf-range>
        bool feature(const ComponentValue& block, const std::vector<const ComponentValue*>& tokens, Atom& atom){
            const std::string text = serialize(trimWhitespace(block.children));
            // boolean
            if (tokens.size() == 1 && tokens[0]->isToken(TokenType::Ident)){
                atom = boolean(lowerAscii(tokens[0]->token.value), text);
                return true;
            }
            // plain "name: value"
            if (tokens.size() >= 3 && tokens[0]->isToken(TokenType::Ident) && tokens[1]->isToken(TokenType::Colon)){
                std::vector<const ComponentValue*> valueTokens(tokens.begin() + 2, tokens.end());
                atom = plain(lowerAscii(tokens[0]->token.value), readValue(valueTokens), text);
                return true;
            }
            // range: name op value | value op name | value op name op value
            std::vector<std::pair<std::vector<const ComponentValue*>, std::string>> parts;   // operands and the operator after them
            std::vector<const ComponentValue*> current;
            for (size_t i = 0; i < tokens.size();){
                std::string op;
                size_t length = 0;
                if (isComparison(tokens, i, op, length)){
                    parts.emplace_back(current, op);
                    current.clear();
                    i += length;
                    continue;
                }
                current.push_back(tokens[i]);
                i++;
            }
            parts.emplace_back(current, std::string());
            if (parts.size() < 2 || parts.size() > 3)
                return false;
            rewritten = true;
            auto nameOf = [](const std::vector<const ComponentValue*>& operand) -> std::string {
                if (operand.size() == 1 && operand[0]->isToken(TokenType::Ident))
                    return lowerAscii(operand[0]->token.value);
                return std::string();
            };
            if (parts.size() == 2){
                std::string name = nameOf(parts[0].first);
                std::string op = parts[0].second;
                Value value;
                if (!name.empty() && nameOf(parts[1].first).empty()){
                    value = readValue(parts[1].first);
                }else{
                    name = nameOf(parts[1].first);
                    value = readValue(parts[0].first);
                    op = flip(op);
                }
                if (name.empty())
                    return false;
                atom = range(name, {{op, value}}, text);
                return true;
            }
            // value op name op value: both operators point the same way
            const std::string name = nameOf(parts[1].first);
            if (name.empty())
                return false;
            const std::string op1 = parts[0].second, op2 = parts[1].second;
            if (op1 == "=" || op2 == "=" || op1[0] != op2[0])
                return false;
            const Value low = readValue(parts[0].first);
            const Value high = readValue(parts[2].first);
            atom = range(name, {{flip(op1), low}, {op2, high}}, text);
            return true;
        }

        static std::string flip(const std::string& op){
            if (op == "<") return ">";
            if (op == "<=") return ">=";
            if (op == ">") return "<";
            if (op == ">=") return "<=";
            return op;
        }

        // the feature's range type, or nothing for discrete features
        static MediaConjunction::Range MediaConjunction::* rangeMember(const std::string& name, Value::Kind& kind, double& step){
            if (name == "width" || name == "height"){
                kind = Value::Kind::Length;
                step = kLengthStep;
                return name == "width" ? &MediaConjunction::width : &MediaConjunction::height;
            }
            if (name == "aspect-ratio"){
                kind = Value::Kind::Ratio;
                step = kRatioStep;
                return &MediaConjunction::aspect;
            }
            if (name == "resolution"){
                kind = Value::Kind::Resolution;
                step = kResolutionStep;
                return &MediaConjunction::resolution;
            }
            return nullptr;
        }

        // name OP value list ("width >= 600px")
        Atom range(const std::string& name, const std::vector<std::pair<std::string, Value>>& constraints, const std::string& text){
            Value::Kind kind = Value::Kind::Invalid;
            double step = 0;
            MediaConjunction::Range MediaConjunction::* member = rangeMember(name, kind, step);
            if (!member){
                // a static numeric feature compared with a range
                std::optional<double> value = staticNumber(name);
                if (!value)
                    return unknown(text);
                bool result = true;
                for (const auto& c : constraints){
                    if (c.second.kind != Value::Kind::Number)
                        return unknown(text);
                    result = result && compare(*value, c.first, c.second.number);
                }
                reportStatic(text, name);
                return staticAtom(result);
            }
            std::optional<double> min, max;
            for (const auto& c : constraints){
                Value v = c.second;
                if (kind == Value::Kind::Ratio && v.kind == Value::Kind::Number){
                    v.kind = Value::Kind::Ratio;
                }else if (kind == Value::Kind::Length && v.kind == Value::Kind::Number && v.number == 0){
                    v.kind = Value::Kind::Length;
                }
                if (v.kind != kind)
                    return unknown(text);
                if (c.first == ">="){
                    min = min ? std::max(*min, v.number) : v.number;
                }else if (c.first == ">"){
                    const double m = v.number + step;
                    min = min ? std::max(*min, m) : m;
                }else if (c.first == "<="){
                    max = max ? std::min(*max, v.number) : v.number;
                }else if (c.first == "<"){
                    const double m = v.number - step;
                    max = max ? std::min(*max, m) : m;
                }else{
                    min = v.number;
                    max = v.number;
                }
            }
            return rangeAtom(member, min, max, step);
        }

        static bool compare(double value, const std::string& op, double against){
            if (op == ">=") return value >= against;
            if (op == ">") return value > against;
            if (op == "<=") return value <= against;
            if (op == "<") return value < against;
            return value == against;
        }

        Atom plain(const std::string& name, const Value& value, const std::string& text){
            if (value.kind == Value::Kind::Invalid)
                return unknown(text);
            // min-/max- prefixed range features
            for (const char* prefix : {"min-", "max-"}){
                const std::string p(prefix);
                if (name.compare(0, p.size(), p) == 0){
                    const std::string base = name.substr(p.size());
                    Value::Kind kind = Value::Kind::Invalid;
                    double step = 0;
                    if (rangeMember(base, kind, step))
                        return range(base, {{p == "min-" ? ">=" : "<=", value}}, text);
                    if (std::optional<double> number = staticNumber(base)){
                        if (value.kind != Value::Kind::Number)
                            return unknown(text);
                        reportStatic(text, base);
                        return staticAtom(p == "min-" ? *number >= value.number : *number <= value.number);
                    }
                }
            }
            Value::Kind kind = Value::Kind::Invalid;
            double step = 0;
            if (rangeMember(name, kind, step))
                return range(name, {{"=", value}}, text);
            if (name == "orientation"){
                if (value.kind != Value::Kind::Ident || (value.ident != "landscape" && value.ident != "portrait"))
                    return unknown(text);
                MediaConjunction c;
                c.orientation = value.ident;
                MediaConjunction n;
                n.orientation = value.ident == "landscape" ? "portrait" : "landscape";
                return Atom{{c}, {n}};
            }
            if (name == "theme"){
                if (value.kind != Value::Kind::Ident)
                    return unknown(text);
                return themeAtom(value.ident);
            }
            if (name == "prefers-color-scheme"){
                if (value.kind != Value::Kind::Ident || (value.ident != "dark" && value.ident != "light"))
                    return unknown(text);
                rewritten = true;
                return themeAtom(value.ident);
            }
            if (name == "prefers-reduced-motion"){
                if (value.kind != Value::Kind::Ident || (value.ident != "reduce" && value.ident != "no-preference"))
                    return unknown(text);
                rewritten = true;
                Atom atom = themeAtom("reduced-motion");
                if (value.ident == "no-preference")
                    std::swap(atom.positive, atom.negative);
                return atom;
            }
            if (std::optional<std::string> ident = staticIdent(name)){
                if (value.kind != Value::Kind::Ident)
                    return unknown(text);
                reportStatic(text, name);
                return staticAtom(value.ident == *ident);
            }
            if (std::optional<double> number = staticNumber(name)){
                if (value.kind != Value::Kind::Number)
                    return unknown(text);
                reportStatic(text, name);
                return staticAtom(*number == value.number);
            }
            return unknown(text);
        }

        Atom boolean(const std::string& name, const std::string& text){
            Value::Kind kind = Value::Kind::Invalid;
            double step = 0;
            if (rangeMember(name, kind, step) || name == "orientation"){
                rewritten = true;
                return staticAtom(true);   // the viewport always has a size
            }
            if (name == "prefers-color-scheme")
                return staticAtom(true);
            if (name == "prefers-reduced-motion"){
                rewritten = true;
                return themeAtom("reduced-motion");
            }
            if (std::optional<std::string> ident = staticIdent(name)){
                reportStatic(text, name);
                return staticAtom(*ident != "none" && *ident != "no-preference");
            }
            if (std::optional<double> number = staticNumber(name)){
                reportStatic(text, name);
                return staticAtom(*number != 0);
            }
            return unknown(text);
        }

        Atom themeAtom(const std::string& theme){
            MediaConjunction positive;
            positive.theme = theme;
            MediaConjunction negative;
            negative.negatedTheme = theme;
            return Atom{{positive}, {negative}};
        }

        std::optional<std::string> staticIdent(const std::string& name) const {
            if (name == "hover" || name == "any-hover")
                return std::string(options.touchDevice ? "none" : "hover");
            if (name == "pointer" || name == "any-pointer")
                return std::string(options.touchDevice ? "coarse" : "fine");
            if (name == "color-gamut")
                return std::string("srgb");
            if (name == "prefers-contrast" || name == "prefers-reduced-transparency")
                return std::string("no-preference");
            if (name == "forced-colors" || name == "inverted-colors" || name == "scripting")
                return std::string("none");
            if (name == "update")
                return std::string("fast");
            if (name == "display-mode")
                return std::string("fullscreen");
            if (name == "overflow-block" || name == "overflow-inline")
                return std::string("scroll");
            if (name == "dynamic-range")
                return std::string("standard");
            return std::nullopt;
        }

        static std::optional<double> staticNumber(const std::string& name){
            if (name == "color")
                return 8.0;
            if (name == "monochrome" || name == "color-index")
                return 0.0;
            return std::nullopt;
        }

        void reportStatic(const std::string& text, const std::string& name){
            rewritten = true;
            std::string value;
            if (std::optional<std::string> ident = staticIdent(name))
                value = *ident;
            else if (std::optional<double> number = staticNumber(name))
                value = formatNumber(*number);
            report(codes::MediaStatic, "'(" + text + ")' is evaluated at load with " + name + ": " + value, "", name);
        }

        Atom unknown(const std::string& text){
            rewritten = true;
            report(codes::MediaFeatureUnsupported, "media feature '(" + text + ")' cannot be evaluated; the query never matches",
                   "use width, height, aspect-ratio, orientation, resolution or prefers-color-scheme", text);
            return staticAtom(false);
        }

        void report(DiagCode code, const std::string& message, const std::string& suggestion, const std::string& subject){
            if (sink)
                sink->report(code, ref, message, suggestion, std::string(), subject);
        }

        const CompatOptions& options;
        DiagnosticSink* sink;
        SourceRef ref;

    public:
        bool emConverted = false;
    };

    std::string canonicalInput(const std::vector<ComponentValue>& query){
        return serialize(trimWhitespace(query));
    }

}

bool MediaConjunction::empty() const{
    return !width.min && !width.max && !height.min && !height.max && !resolution.min && !resolution.max && !aspect.min &&
           !aspect.max && orientation.empty() && theme.empty() && negatedTheme.empty() && !contradiction && !inexpressible;
}

namespace {

    void intersect(MediaConjunction::Range& target, const MediaConjunction::Range& other, bool& contradiction){
        if (other.min)
            target.min = target.min ? std::max(*target.min, *other.min) : *other.min;
        if (other.max)
            target.max = target.max ? std::min(*target.max, *other.max) : *other.max;
        if (target.min && target.max && *target.min > *target.max + 1e-9)
            contradiction = true;
    }

}

MediaConjunction MediaConjunction::conjoin(const MediaConjunction& a, const MediaConjunction& b){
    MediaConjunction c = a;
    c.contradiction = a.contradiction || b.contradiction;
    c.inexpressible = a.inexpressible || b.inexpressible;
    intersect(c.width, b.width, c.contradiction);
    intersect(c.height, b.height, c.contradiction);
    intersect(c.resolution, b.resolution, c.contradiction);
    intersect(c.aspect, b.aspect, c.contradiction);
    if (!b.orientation.empty()){
        if (!c.orientation.empty() && c.orientation != b.orientation)
            c.contradiction = true;
        c.orientation = b.orientation;
    }
    if (!b.theme.empty()){
        if (!c.theme.empty() && c.theme != b.theme)
            c.inexpressible = true;   // two themes may be active at once, RmlUi takes one per block
        c.theme = b.theme;
    }
    if (!b.negatedTheme.empty()){
        if (!c.negatedTheme.empty() && c.negatedTheme != b.negatedTheme)
            c.inexpressible = true;
        c.negatedTheme = b.negatedTheme;
    }
    if (!c.theme.empty() && c.theme == c.negatedTheme)
        c.contradiction = true;
    return c;
}

RcssMediaCondition MediaConjunction::toRcss() const{
    RcssMediaCondition condition;
    auto range = [&](const char* name, const Range& r, const char* unit){
        auto value = [&](double v){ return formatNumber(v, 3) + unit; };
        if (r.min && r.max && std::fabs(*r.min - *r.max) < 1e-9){
            condition.features.push_back(std::string(name) + ": " + value(*r.min));
            return;
        }
        if (r.min)
            condition.features.push_back(std::string("min-") + name + ": " + value(*r.min));
        if (r.max)
            condition.features.push_back(std::string("max-") + name + ": " + value(*r.max));
    };
    range("width", width, "px");
    range("height", height, "px");
    if (aspect.min && aspect.max && std::fabs(*aspect.min - *aspect.max) < 1e-9){
        condition.features.push_back("aspect-ratio: " + ratioText(*aspect.min));
    }else{
        if (aspect.min)
            condition.features.push_back("min-aspect-ratio: " + ratioText(*aspect.min));
        if (aspect.max)
            condition.features.push_back("max-aspect-ratio: " + ratioText(*aspect.max));
    }
    range("resolution", resolution, "x");
    if (!orientation.empty())
        condition.features.push_back("orientation: " + orientation);
    if (!theme.empty())
        condition.features.push_back("theme: " + theme);
    if (!negatedTheme.empty()){
        condition.negated = true;
        condition.features.push_back("theme: " + negatedTheme);
    }
    return condition;
}

MediaResult MediaResult::unconditional(){
    MediaResult result;
    result.always = true;
    return result;
}

MediaResult MediaResult::conjoin(const MediaResult& a, const MediaResult& b){
    if (a.never || b.never){
        MediaResult result;
        result.never = true;
        return result;
    }
    if (a.always)
        return b;
    if (b.always)
        return a;
    MediaResult result;
    for (const MediaConjunction& x : a.blocks){
        for (const MediaConjunction& y : b.blocks){
            MediaConjunction c = MediaConjunction::conjoin(x, y);
            if (c.contradiction || c.inexpressible)
                continue;
            if (!c.negatedTheme.empty() && c.toRcss().features.size() > 1)
                continue;
            result.blocks.push_back(std::move(c));
        }
    }
    if (result.blocks.empty())
        result.never = true;
    return result;
}

MediaResult MediaQuery::normalize(const std::vector<ComponentValue>& prelude, SourceRef ref, const CompatOptions& options,
                                  DiagnosticSink* sink){
    MediaResult result;
    std::vector<std::vector<ComponentValue>> queries = splitAtCommas(prelude);
    bool anyAlways = false;
    std::vector<std::string> seen;
    for (const std::vector<ComponentValue>& query : queries){
        Normalizer normalizer(options, sink, ref);
        Dnf dnf;
        normalizer.query(query, dnf);
        bool reportedInexpressible = false;
        size_t kept = 0;
        std::vector<std::string> texts;
        for (MediaConjunction& conjunction : dnf){
            if (conjunction.contradiction)
                continue;
            const RcssMediaCondition rcss = conjunction.toRcss();
            if (conjunction.inexpressible || (conjunction.negatedTheme.size() && rcss.features.size() > 1)){
                if (!reportedInexpressible && sink){
                    sink->report(codes::MediaFeatureUnsupported, ref,
                                 "media query '" + canonicalInput(query) + "' combines conditions RmlUi cannot express in one block; that part never matches",
                                 "split the rules into separate @media blocks", std::string(), canonicalInput(query));
                }
                reportedInexpressible = true;
                continue;
            }
            if (conjunction.empty()){
                anyAlways = true;
                texts.push_back("");
                kept++;
                continue;
            }
            const std::string text = rcss.text();
            texts.push_back(text);
            if (std::find(seen.begin(), seen.end(), text) == seen.end()){
                seen.push_back(text);
                result.blocks.push_back(conjunction);
            }
            kept++;
        }
        // AT-0005 once per query that RmlUi does not read as written
        const std::string input = canonicalInput(query);
        bool same = texts.size() == 1 && texts[0] == input && !normalizer.rewritten && !normalizer.emConverted;
        if (texts.empty())
            same = false;
        if (!same && sink && kept > 0){
            std::string output;
            for (const std::string& t : texts){
                if (!output.empty())
                    output += " | ";
                output += t.empty() ? "all" : t;
            }
            sink->report(codes::MediaNormalized, ref, "media query '" + input + "' is written for RmlUi as '" + output + "'",
                         std::string(), std::string(), input);
        }
    }
    if (anyAlways){
        result.always = true;
        result.blocks.clear();
        return result;
    }
    if (result.blocks.empty())
        result.never = true;
    return result;
}

MediaResult MediaQuery::normalize(std::string_view text, const CompatOptions& options, DiagnosticSink* sink, SourceRef ref){
    std::vector<ComponentValue> values = parseComponentValueList(text, ref.offset);
    return normalize(values, ref, options, sink);
}

std::vector<std::string> MediaQuery::blockTexts(const MediaResult& result){
    std::vector<std::string> texts;
    if (result.always){
        texts.push_back("all");
        return texts;
    }
    for (const MediaConjunction& block : result.blocks)
        texts.push_back(block.toRcss().text());
    return texts;
}
