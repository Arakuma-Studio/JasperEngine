// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effect parameter values (phase-5 spec 3.3), read with the compatibility layer's CSS parser
// and colour parser, so a value means what it means in CSS whether it is written in effect()
// or reaches the engine through a var().

#include "EffectValue.h"

#include "htmlui/compat/Arena.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    bool isWhitespace(const hc::ComponentValue& v){
        return v.isToken(hc::TokenType::Whitespace) || v.isToken(hc::TokenType::Comment);
    }

    // the component values of a text without white space, in the current arena
    std::vector<const hc::ComponentValue*> wordsOf(const hc::ValueList& list){
        std::vector<const hc::ComponentValue*> words;
        for (const hc::ComponentValue& v : list){
            if (!isWhitespace(v))
                words.push_back(&v);
        }
        return words;
    }

    std::string lower(std::string_view text){
        return hc::lowerAscii(text);
    }

    std::string identOf(const hc::ComponentValue& v){
        return v.isToken(hc::TokenType::Ident) ? lower(v.token.value.view()) : std::string();
    }

    bool numberOf(const hc::ComponentValue& v, float& out){
        if (!v.isToken(hc::TokenType::Number))
            return false;
        out = (float)v.token.number;
        return true;
    }

    std::string unitOf(const hc::ComponentValue& v){
        return v.isToken(hc::TokenType::Dimension) ? lower(v.token.unit.view()) : std::string();
    }

    bool parseNumber(const std::vector<const hc::ComponentValue*>& w, float& out, std::string& error){
        if (w.size() != 1 || !numberOf(*w[0], out)){
            error = "expected a number";
            return false;
        }
        return true;
    }

    bool parseLength(const hc::ComponentValue& v, EffectLength& out, std::string& error){
        if (v.isToken(hc::TokenType::Number)){
            if (v.token.number != 0.0){
                error = "a length needs a unit (px, em, rem, vw, vh, vmin, vmax or %)";
                return false;
            }
            out.number = 0.f;
            out.unit = EffectLength::Unit::Px;
            return true;
        }
        if (v.isToken(hc::TokenType::Percentage)){
            out.number = (float)v.token.number;
            out.unit = EffectLength::Unit::Percent;
            return true;
        }
        const std::string unit = unitOf(v);
        if (unit.empty()){
            error = "expected a length";
            return false;
        }
        out.number = (float)v.token.number;
        if (unit == "px") out.unit = EffectLength::Unit::Px;
        else if (unit == "em") out.unit = EffectLength::Unit::Em;
        else if (unit == "rem") out.unit = EffectLength::Unit::Rem;
        else if (unit == "vw") out.unit = EffectLength::Unit::Vw;
        else if (unit == "vh") out.unit = EffectLength::Unit::Vh;
        else if (unit == "vmin") out.unit = EffectLength::Unit::Vmin;
        else if (unit == "vmax") out.unit = EffectLength::Unit::Vmax;
        else{
            error = "'" + unit + "' is not an effect length unit (px, em, rem, vw, vh, vmin, vmax or %)";
            return false;
        }
        return true;
    }

    bool parseAngle(const hc::ComponentValue& v, float& out, std::string& error){
        float number = 0.f;
        if (numberOf(v, number)){
            out = number * kPi / 180.f;   // a bare number is degrees
            return true;
        }
        const std::string unit = unitOf(v);
        const float value = (float)v.token.number;
        if (unit == "deg") out = value * kPi / 180.f;
        else if (unit == "rad") out = value;
        else if (unit == "grad") out = value * kPi / 200.f;
        else if (unit == "turn") out = value * 2.f * kPi;
        else{
            error = "expected an angle (deg, rad, grad, turn, or a number of degrees)";
            return false;
        }
        return true;
    }

    bool parseTime(const hc::ComponentValue& v, float& out, std::string& error){
        const std::string unit = unitOf(v);
        if (unit == "s") out = (float)v.token.number;
        else if (unit == "ms") out = (float)v.token.number / 1000.f;
        else if (v.isToken(hc::TokenType::Number) && v.token.number == 0.0) out = 0.f;
        else{
            error = "expected a time (s or ms)";
            return false;
        }
        return true;
    }

    bool parseColor(const hc::ComponentValue& v, EffectValue& out, std::string& error){
        hc::ColorParseInfo info;
        std::optional<hc::CssColor> color = hc::ColorValue::parse(v, info);
        if (info.currentColor){
            out.currentColor = true;
            out.color = Vector4(0.f, 0.f, 0.f, 1.f);
            return true;
        }
        if (!color){
            error = info.error.empty() ? std::string("expected a colour") : info.error;
            return false;
        }
        double srgb[4];
        hc::ColorValue::toSrgb(*color, srgb);
        auto channel = [](double c){ return EffectValueParser::srgbToLinear((float)std::min(std::max(c, 0.0), 1.0)); };
        out.color = Vector4(channel(srgb[0]), channel(srgb[1]), channel(srgb[2]), (float)std::min(std::max(srgb[3], 0.0), 1.0));
        out.currentColor = false;
        return true;
    }

    // a length-percentage of a position axis: fraction + px (px lengths only)
    bool positionLength(const hc::ComponentValue& v, EffectPositionAxis& axis){
        if (v.isToken(hc::TokenType::Percentage)){
            axis.fraction = (float)v.token.number / 100.f;
            axis.px = 0.f;
            return true;
        }
        if (v.isToken(hc::TokenType::Number) && v.token.number == 0.0){
            axis.fraction = 0.f;
            axis.px = 0.f;
            return true;
        }
        if (unitOf(v) == "px"){
            axis.fraction = 0.f;
            axis.px = (float)v.token.number;
            return true;
        }
        // calc(P% + Lpx) / calc(P% - Lpx), the canonical form of a combined axis
        if (v.kind == hc::ComponentValue::Kind::Function && lower(v.name().view()) == "calc"){
            std::vector<const hc::ComponentValue*> w = wordsOf(v.children);
            if (w.size() == 3 && w[0]->isToken(hc::TokenType::Percentage) && w[1]->isToken(hc::TokenType::Delim) &&
                (w[1]->token.isDelim('+') || w[1]->token.isDelim('-')) && unitOf(*w[2]) == "px"){
                axis.fraction = (float)w[0]->token.number / 100.f;
                axis.px = (float)w[2]->token.number * (w[1]->token.isDelim('-') ? -1.f : 1.f);
                return true;
            }
        }
        return false;
    }

    bool edgeKeyword(const std::string& k, bool& horizontal, float& fraction){
        if (k == "left"){ horizontal = true; fraction = 0.f; return true; }
        if (k == "right"){ horizontal = true; fraction = 1.f; return true; }
        if (k == "top"){ horizontal = false; fraction = 0.f; return true; }
        if (k == "bottom"){ horizontal = false; fraction = 1.f; return true; }
        return false;
    }

    // CSS <position>: 1-2 values, or the 3-4 value keyword-offset form
    bool parsePosition(const std::vector<const hc::ComponentValue*>& w, EffectPositionAxis& x, EffectPositionAxis& y, std::string& error){
        x = EffectPositionAxis{0.5f, 0.f};
        y = EffectPositionAxis{0.5f, 0.f};
        error = "expected a position (keywords left, center, right, top, bottom, or % and px values)";
        if (w.empty() || w.size() > 4)
            return false;
        if (w.size() <= 2){
            // each value's kind: H (left, right), V (top, bottom), C (center), L (a length-percentage)
            enum Kind { H, V, C, L };
            Kind kinds[2];
            EffectPositionAxis axes[2];
            for (size_t i = 0; i < w.size(); i++){
                const std::string k = identOf(*w[i]);
                bool horizontal = false;
                float fraction = 0.f;
                if (k == "center"){
                    kinds[i] = C;
                    axes[i] = EffectPositionAxis{0.5f, 0.f};
                }else if (edgeKeyword(k, horizontal, fraction)){
                    kinds[i] = horizontal ? H : V;
                    axes[i] = EffectPositionAxis{fraction, 0.f};
                }else if (k.empty() && positionLength(*w[i], axes[i])){
                    kinds[i] = L;
                }else{
                    return false;
                }
            }
            if (w.size() == 1){
                if (kinds[0] == V)
                    y = axes[0];
                else
                    x = axes[0];
                return true;
            }
            // x then y; two keywords may come the other way round (top left)
            if (kinds[0] != V && kinds[1] != H){
                x = axes[0];
                y = axes[1];
                return true;
            }
            if ((kinds[0] == V || kinds[0] == C) && (kinds[1] == H || kinds[1] == C)){
                x = axes[1];
                y = axes[0];
                return true;
            }
            return false;
        }
        // 3-4 values: <keyword> [<offset>] <keyword> [<offset>]
        bool xSet = false, ySet = false;
        for (size_t i = 0; i < w.size();){
            const std::string k = identOf(*w[i]);
            bool horizontal = false;
            float fraction = 0.f;
            EffectPositionAxis axis;
            if (k == "center"){
                // center takes no offset; it is the axis not otherwise named
                i++;
                continue;
            }
            if (!edgeKeyword(k, horizontal, fraction))
                return false;
            axis.fraction = fraction;
            i++;
            EffectPositionAxis offset;
            if (i < w.size() && identOf(*w[i]).empty() && positionLength(*w[i], offset)){
                if (offset.fraction != 0.f && fraction != 0.f){
                    axis.fraction = 1.f - offset.fraction;
                    axis.px = -offset.px;
                }else if (fraction != 0.f){
                    axis.px = -offset.px;
                }else{
                    axis.fraction = offset.fraction;
                    axis.px = offset.px;
                }
                i++;
            }
            if (horizontal){
                if (xSet) return false;
                x = axis;
                xSet = true;
            }else{
                if (ySet) return false;
                y = axis;
                ySet = true;
            }
        }
        return true;
    }

    bool parseBoolean(const hc::ComponentValue& v, float& out){
        const std::string k = identOf(v);
        if (k == "on" || k == "true" || k == "yes"){ out = 1.f; return true; }
        if (k == "off" || k == "false" || k == "no"){ out = 0.f; return true; }
        float n = 0.f;
        if (numberOf(v, n) && (n == 0.f || n == 1.f)){
            out = n;
            return true;
        }
        return false;
    }

    // 2-8 entries "<color> [<percentage>]"; missing positions spread as CSS gradients do
    bool parsePalette(const std::vector<const hc::ComponentValue*>& w, EffectValue& out, std::string& error){
        struct Entry { Vector4 color; float position; bool hasPosition; };
        std::vector<Entry> entries;
        for (size_t i = 0; i < w.size(); i++){
            EffectValue color;
            std::string colorError;
            if (!parseColor(*w[i], color, colorError) || color.currentColor){
                error = color.currentColor ? "a palette takes colours, not currentColor" : "expected a colour in the palette: " + colorError;
                return false;
            }
            Entry entry{color.color, 0.f, false};
            if (i + 1 < w.size() && w[i + 1]->isToken(hc::TokenType::Percentage)){
                entry.position = (float)w[i + 1]->token.number / 100.f;
                entry.hasPosition = true;
                i++;
            }
            entries.push_back(entry);
        }
        if (entries.size() < 2 || entries.size() > 8){
            error = "a palette has 2 to 8 colours (it has " + std::to_string(entries.size()) + ")";
            return false;
        }
        // CSS gradient rules: first and last default to 0% and 100%, a position below an
        // earlier one is an error here (positions must not decrease), gaps are spread evenly
        if (!entries.front().hasPosition){
            entries.front().position = 0.f;
            entries.front().hasPosition = true;
        }
        if (!entries.back().hasPosition){
            entries.back().position = std::max(1.f, entries.front().position);
            entries.back().hasPosition = true;
        }
        float last = entries.front().position;
        for (const Entry& entry : entries){
            if (entry.hasPosition){
                if (entry.position < last){
                    error = "palette positions must not decrease";
                    return false;
                }
                last = entry.position;
            }
        }
        for (size_t i = 1; i < entries.size();){
            if (entries[i].hasPosition){
                i++;
                continue;
            }
            size_t j = i;
            while (!entries[j].hasPosition)
                j++;
            const float a = entries[i - 1].position;
            const float b = entries[j].position;
            for (size_t k = i; k < j; k++){
                entries[k].position = a + (b - a) * float(k - i + 1) / float(j - i + 1);
                entries[k].hasPosition = true;
            }
            i = j;
        }
        out.palette.clear();
        for (const Entry& entry : entries)
            out.palette.push_back({entry.color, std::min(std::max(entry.position, 0.f), 1.f)});
        return true;
    }

    const char* unitName(EffectLength::Unit unit){
        switch (unit){
            case EffectLength::Unit::Px: return "px";
            case EffectLength::Unit::Em: return "em";
            case EffectLength::Unit::Rem: return "rem";
            case EffectLength::Unit::Vw: return "vw";
            case EffectLength::Unit::Vh: return "vh";
            case EffectLength::Unit::Vmin: return "vmin";
            case EffectLength::Unit::Vmax: return "vmax";
            case EffectLength::Unit::Percent: return "%";
        }
        return "px";
    }

    std::string axisText(const EffectPositionAxis& axis){
        const float percent = axis.fraction * 100.f;
        if (axis.px == 0.f)
            return EffectValueParser::formatNumber(percent) + "%";
        if (axis.fraction == 0.f)
            return EffectValueParser::formatNumber(axis.px) + "px";
        return "calc(" + EffectValueParser::formatNumber(percent) + "% " + (axis.px < 0.f ? "- " : "+ ") +
               EffectValueParser::formatNumber(std::fabs(axis.px)) + "px)";
    }

}

bool EffectValueParser::hasVar(std::string_view css){
    const std::string text = lower(css);
    return text.find("var(") != std::string::npos;
}

bool EffectValueParser::parse(const EffectParamDef& def, std::string_view css, EffectValue& out, std::string& error){
    return parseType(def.type, def.options, css, out, error);
}

bool EffectValueParser::parseType(EffectParamType type, const std::vector<std::string>& options, std::string_view css, EffectValue& out,
                                  std::string& error){
    out = EffectValue();
    out.type = type;
    out.text = std::string(css);
    error.clear();
    if (hasVar(css)){
        // resolved when the element's custom properties are known (RmlUi's var() substitution)
        out.isVar = true;
        return true;
    }

    hc::Arena arena;
    hc::ArenaScope scope(arena);
    std::vector<hc::ParseError> errors;
    const hc::ValueList list = hc::parseComponentValueList(css, 0, {}, &errors);
    const std::vector<const hc::ComponentValue*> w = wordsOf(list);
    if (w.empty()){
        error = "the value is empty";
        return false;
    }

    switch (type){
        case EffectParamType::Number:
            return parseNumber(w, out.f, error);
        case EffectParamType::Integer: {
            if (!parseNumber(w, out.f, error))
                return false;
            out.f = std::round(out.f);
            return true;
        }
        case EffectParamType::Percentage: {
            if (w.size() == 1 && w[0]->isToken(hc::TokenType::Percentage)){
                out.f = (float)w[0]->token.number / 100.f;
                return true;
            }
            float n = 0.f;
            if (w.size() == 1 && numberOf(*w[0], n)){
                if (n < 0.f || n > 1.f){
                    error = "a percentage written as a number is 0 to 1 (write " + formatNumber(n) + "%)";
                    return false;
                }
                out.f = n;
                return true;
            }
            error = "expected a percentage";
            return false;
        }
        case EffectParamType::Length: {
            if (w.size() != 1)
                return (error = "expected one length", false);
            if (!parseLength(*w[0], out.length, error))
                return false;
            if (out.length.unit == EffectLength::Unit::Px)
                out.f = out.length.number;
            return true;
        }
        case EffectParamType::Angle:
            if (w.size() != 1)
                return (error = "expected one angle", false);
            return parseAngle(*w[0], out.f, error);
        case EffectParamType::Time:
            if (w.size() != 1)
                return (error = "expected one time", false);
            return parseTime(*w[0], out.f, error);
        case EffectParamType::Color:
            if (w.size() != 1)
                return (error = "expected one colour", false);
            return parseColor(*w[0], out, error);
        case EffectParamType::Position:
            return parsePosition(w, out.x, out.y, error);
        case EffectParamType::Enum: {
            const std::string k = w.size() == 1 ? identOf(*w[0]) : std::string();
            for (size_t i = 0; i < options.size(); i++){
                if (!k.empty() && k == options[i]){
                    out.f = (float)i;
                    return true;
                }
            }
            std::string list;
            for (const std::string& option : options)
                list += (list.empty() ? "" : ", ") + option;
            error = "expected one of " + list;
            return false;
        }
        case EffectParamType::Boolean:
            if (w.size() == 1 && parseBoolean(*w[0], out.f))
                return true;
            error = "expected on, off, true, false, yes, no, 1 or 0";
            return false;
        case EffectParamType::Palette:
            return parsePalette(w, out, error);
    }
    error = "unknown parameter type";
    return false;
}

float EffectValueParser::clampToRange(const EffectParamDef& def, float v, bool& clamped){
    clamped = false;
    if (!def.hasRange)
        return v;
    const float c = std::min(std::max(v, def.minValue), def.maxValue);
    clamped = (c != v);
    return c;
}

bool EffectValueParser::clamp(const EffectParamDef& def, EffectValue& value){
    if (value.isVar || !def.hasRange)
        return false;
    switch (def.type){
        case EffectParamType::Number:
        case EffectParamType::Integer:
        case EffectParamType::Percentage:
        case EffectParamType::Angle:
        case EffectParamType::Time: {
            bool clamped = false;
            value.f = clampToRange(def, value.f, clamped);
            return clamped;
        }
        case EffectParamType::Length: {
            // px lengths now, the others once resolved
            if (value.length.unit != EffectLength::Unit::Px)
                return false;
            bool clamped = false;
            value.length.number = clampToRange(def, value.length.number, clamped);
            value.f = value.length.number;
            return clamped;
        }
        default:
            return false;
    }
}

float EffectValueParser::resolveLength(const EffectLength& length, const EffectLengthContext& context){
    switch (length.unit){
        case EffectLength::Unit::Px: return length.number;
        case EffectLength::Unit::Em: return length.number * context.fontSize;
        case EffectLength::Unit::Rem: return length.number * context.rootFontSize;
        case EffectLength::Unit::Vw: return length.number * context.viewport.x / 100.f;
        case EffectLength::Unit::Vh: return length.number * context.viewport.y / 100.f;
        case EffectLength::Unit::Vmin: return length.number * std::min(context.viewport.x, context.viewport.y) / 100.f;
        case EffectLength::Unit::Vmax: return length.number * std::max(context.viewport.x, context.viewport.y) / 100.f;
        case EffectLength::Unit::Percent: return length.number * context.areaShorterSide / 100.f;
    }
    return length.number;
}

float EffectValueParser::srgbToLinear(float c){
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float EffectValueParser::linearToSrgb(float c){
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

std::string EffectValueParser::formatNumber(float value){
    if (value == 0.f)
        return "0";
    if (std::isnan(value) || std::isinf(value))
        return "0";
    char buffer[64];
    for (int precision = 1; precision <= 9; precision++){
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, (double)value);
        if (std::strtof(buffer, nullptr) == value)
            break;
    }
    std::string text = buffer;
    // no exponent in CSS output for the values effects use
    if (text.find('e') != std::string::npos){
        std::snprintf(buffer, sizeof(buffer), "%.9f", (double)value);
        text = buffer;
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text;
}

std::string EffectValueParser::colorHex(const Vector4& linear){
    auto byte = [](float c){
        const float v = std::min(std::max(c, 0.f), 1.f);
        return (unsigned int)std::lround(v * 255.f);
    };
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", byte(linearToSrgb(linear.x)), byte(linearToSrgb(linear.y)),
                  byte(linearToSrgb(linear.z)), byte(linear.w));
    return buffer;
}

std::string EffectValueParser::canonical(const EffectParamDef& def, const EffectValue& v){
    if (v.isVar)
        return v.text;
    switch (def.type){
        case EffectParamType::Number:
        case EffectParamType::Integer:
            return formatNumber(v.f);
        case EffectParamType::Percentage:
            return formatNumber(v.f * 100.f) + "%";
        case EffectParamType::Length:
            if (v.length.number == 0.f && v.length.unit == EffectLength::Unit::Px)
                return "0px";
            return formatNumber(v.length.number) + unitName(v.length.unit);
        case EffectParamType::Angle:
            return formatNumber(v.f * 180.f / kPi) + "deg";
        case EffectParamType::Time:
            return formatNumber(v.f) + "s";
        case EffectParamType::Color:
            return v.currentColor ? std::string("currentcolor") : colorHex(v.color);
        case EffectParamType::Position:
            return axisText(v.x) + " " + axisText(v.y);
        case EffectParamType::Enum: {
            const int index = (int)std::lround(v.f);
            return (index >= 0 && index < (int)def.options.size()) ? def.options[index] : std::string();
        }
        case EffectParamType::Boolean:
            return v.f != 0.f ? "true" : "false";
        case EffectParamType::Palette: {
            std::string text;
            for (const auto& entry : v.palette){
                if (!text.empty())
                    text += ' ';
                text += colorHex(entry.first) + " " + formatNumber(entry.second * 100.f) + "%";
            }
            return text;
        }
    }
    return v.text;
}

std::string EffectValueParser::expected(const EffectParamDef& def){
    std::string range;
    if (def.hasRange){
        auto bound = [&](float value) -> std::string {
            switch (def.type){
                case EffectParamType::Angle: return formatNumber(value * 180.f / kPi) + "deg";
                case EffectParamType::Time: return formatNumber(value) + "s";
                case EffectParamType::Length: return formatNumber(value) + "px";
                case EffectParamType::Percentage: return formatNumber(value * 100.f) + "%";
                default: return formatNumber(value);
            }
        };
        range = " in [" + bound(def.minValue) + ", " + bound(def.maxValue) + "]";
    }
    switch (def.type){
        case EffectParamType::Number: return "a number" + range;
        case EffectParamType::Integer: return "an integer" + range;
        case EffectParamType::Percentage: return "a percentage" + range;
        case EffectParamType::Length: return "a length (px, em, rem, vw, vh, vmin, vmax or % of the shorter side)" + range;
        case EffectParamType::Angle: return "an angle (deg, rad, grad, turn)" + range;
        case EffectParamType::Time: return "a time (s, ms)" + range;
        case EffectParamType::Color: return "a colour";
        case EffectParamType::Position: return "a position (e.g. 50% 50%, right 10px top 20%)";
        case EffectParamType::Enum: {
            std::string list;
            for (const std::string& option : def.options)
                list += (list.empty() ? "" : ", ") + option;
            return "one of " + list;
        }
        case EffectParamType::Boolean: return "on or off";
        case EffectParamType::Palette: return "2 to 8 colours, each with an optional percentage";
    }
    return "a value";
}
