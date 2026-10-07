// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 core/values.js (decomposeRawValue, getRelativeValue), core/colors.js
// and the matching regular expressions of core/consts.js (MIT). The regular expressions keep
// Anime's ECMAScript syntax: std::regex's default grammar is ECMAScript.

#include "AnimValue.h"

#include "AnimCommon.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>

using namespace jasperengine;

namespace {

    const std::regex& hexTestRgx(){
        static const std::regex r("(^#([\\da-f]{3}){1,2}$)|(^#([\\da-f]{4}){1,2}$)", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& validRgbHslRgx(){
        static const std::regex r("\\)\\s*[-.\\d]", std::regex::ECMAScript);
        return r;
    }
    const std::regex& rgbExecRgx(){
        static const std::regex r("rgb\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\)", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& rgbaExecRgx(){
        static const std::regex r("rgba\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(-?\\d+|-?\\d*.\\d+)\\s*\\)", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& hslExecRgx(){
        static const std::regex r("hsl\\(\\s*(-?\\d+|-?\\d*.\\d+)\\s*,\\s*(-?\\d+|-?\\d*.\\d+)%\\s*,\\s*(-?\\d+|-?\\d*.\\d+)%\\s*\\)", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& hslaExecRgx(){
        static const std::regex r("hsla\\(\\s*(-?\\d+|-?\\d*.\\d+)\\s*,\\s*(-?\\d+|-?\\d*.\\d+)%\\s*,\\s*(-?\\d+|-?\\d*.\\d+)%\\s*,\\s*(-?\\d+|-?\\d*.\\d+)\\s*\\)", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& digitWithExponentRgx(){
        static const std::regex r("[-+]?\\d*\\.?\\d+(?:e[-+]?\\d)?", std::regex::ECMAScript | std::regex::icase);
        return r;
    }
    const std::regex& unitsExecRgx(){
        static const std::regex r("^([-+]?\\d*\\.?\\d+(?:e[-+]?\\d+)?)([a-z]+|%)$", std::regex::ECMAScript | std::regex::icase);
        return r;
    }

    bool startsWith(const std::string& text, const char* prefix){
        return text.rfind(prefix, 0) == 0;
    }

    double hexByte(char a, char b){
        auto digit = [](char c){
            c = (char)std::tolower((unsigned char)c);
            return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
        };
        return digit(a) * 16 + digit(b);
    }

    // +(x).toFixed(3)
    double toFixed3(double x){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.3f", x);
        return std::strtod(buffer, nullptr);
    }

    AnimColor hexToRgba(const std::string& hex){
        const size_t length = hex.size();
        const bool isShort = length == 4 || length == 5;
        AnimColor c;
        c[0] = hexByte(hex[1], hex[isShort ? 1 : 2]);
        c[1] = hexByte(hex[isShort ? 2 : 3], hex[isShort ? 2 : 4]);
        c[2] = hexByte(hex[isShort ? 3 : 5], hex[isShort ? 3 : 6]);
        c[3] = (length == 5 || length == 9) ? toFixed3(hexByte(hex[isShort ? 4 : 7], hex[isShort ? 4 : 8]) / 255) : 1;
        return c;
    }

    double hue2rgb(double p, double q, double t){
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        return t < 1.0 / 6 ? p + (q - p) * 6 * t :
               t < 1.0 / 2 ? q :
               t < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t) * 6 :
               p;
    }

}

bool AnimRawValue::isTruthy() const{
    switch (kind){
        case Kind::NIL: return false;
        case Kind::NUMBER: return number != 0.0 && !std::isnan(number);
        case Kind::STRING: return !text.empty();
    }
    return false;
}

double AnimRawValue::toNumber() const{
    switch (kind){
        case Kind::NIL: return std::nan("");
        case Kind::NUMBER: return number;
        case Kind::STRING: return AnimMath::toNumber(text);
    }
    return std::nan("");
}

std::string AnimRawValue::toString() const{
    switch (kind){
        case Kind::NIL: return "";
        case Kind::NUMBER: return AnimMath::toString(number);
        case Kind::STRING: return text;
    }
    return "";
}

bool AnimRawValue::operator==(const AnimRawValue& other) const{
    if (kind != other.kind)
        return false;
    if (kind == Kind::NUMBER)
        return number == other.number;
    if (kind == Kind::STRING)
        return text == other.text;
    return true;
}

AnimDecomposed AnimValue::decompose(const AnimRawValue& raw){
    AnimDecomposed out;
    if (!raw.isTruthy())
        return out;
    const double num = raw.toNumber();
    if (!std::isnan(num)){
        out.n = num;
        return out;
    }
    std::string str = raw.text;
    // relative operators (+=, -=, *=)
    if (str.size() > 1 && str[1] == '='){
        out.o = str[0];
        str = str.substr(2);
    }
    double number = 0;
    std::string unit;
    if (str.find(' ') == std::string::npos && splitUnit(str, number, unit)){
        out.t = AnimValueType::UNIT;
        out.n = number;
        out.u = unit;
        return out;
    } else if (out.o){
        out.n = AnimMath::toNumber(str);
        return out;
    } else if (isColor(str)){
        out.t = AnimValueType::COLOR;
        AnimColor c = parseColor(str);
        out.d = std::vector<double>(c.begin(), c.end());
        return out;
    } else {
        // complex string (svg coordinates, calc(), filters ...)
        out.t = AnimValueType::COMPLEX;
        std::vector<double> numbers;
        std::vector<std::string> strings;
        splitComplex(str, numbers, strings);
        out.d = numbers;
        out.s = strings;
        return out;
    }
}

double AnimValue::relative(double x, double y, char op){
    return op == '-' ? x - y :
           op == '+' ? x + y :
           x * y;
}

bool AnimValue::isHex(const std::string& text){
    return std::regex_search(text, hexTestRgx());
}

bool AnimValue::isColor(const std::string& text){
    if (isHex(text))
        return true;
    // 'rgb(255, 0, 0) 0px 0px 6px 0px' (box-shadow) is not a colour
    return (startsWith(text, "rgb") || startsWith(text, "hsl")) &&
           (text.back() == ')' || !std::regex_search(text, validRgbHslRgx()));
}

AnimColor AnimValue::parseColor(const std::string& text){
    std::smatch m;
    if (startsWith(text, "rgb")){
        if (std::regex_search(text, m, rgbExecRgx()) || std::regex_search(text, m, rgbaExecRgx())){
            const double a = m.size() > 4 && m[4].matched ? AnimMath::toNumber(m[4].str()) : 1;
            return {AnimMath::toNumber(m[1].str()), AnimMath::toNumber(m[2].str()), AnimMath::toNumber(m[3].str()), a};
        }
        return {0, 0, 0, 1};
    }
    if (isHex(text))
        return hexToRgba(text);
    if (startsWith(text, "hsl")){
        if (std::regex_search(text, m, hslExecRgx()) || std::regex_search(text, m, hslaExecRgx())){
            const double h = AnimMath::toNumber(m[1].str()) / 360;
            const double s = AnimMath::toNumber(m[2].str()) / 100;
            const double l = AnimMath::toNumber(m[3].str()) / 100;
            const double a = m.size() > 4 && m[4].matched ? AnimMath::toNumber(m[4].str()) : 1;
            double r, g, b;
            if (s == 0){
                r = g = b = l;
            } else {
                const double q = l < .5 ? l * (1 + s) : l + s - l * s;
                const double p = 2 * l - q;
                r = AnimMath::round(hue2rgb(p, q, h + 1.0 / 3) * 255, 0);
                g = AnimMath::round(hue2rgb(p, q, h) * 255, 0);
                b = AnimMath::round(hue2rgb(p, q, h - 1.0 / 3) * 255, 0);
            }
            return {r, g, b, a};
        }
        return {0, 0, 0, 1};
    }
    return {0, 0, 0, 1};
}

std::string AnimValue::colorToRgbaString(const AnimColor& c){
    return "rgba(" + AnimMath::toString(AnimMath::round(c[0], 0)) + "," + AnimMath::toString(AnimMath::round(c[1], 0)) + "," +
           AnimMath::toString(AnimMath::round(c[2], 0)) + "," + AnimMath::toString(c[3]) + ")";
}

std::string AnimValue::colorToHexString(const AnimColor& c){
    auto channel = [](double v){
        return (int)AnimMath::clamp(AnimMath::round(v, 0), 0, 255);
    };
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", channel(c[0]), channel(c[1]), channel(c[2]), channel(c[3] * 255));
    return buffer;
}

bool AnimValue::splitUnit(const std::string& text, double& number, std::string& unit){
    std::smatch m;
    if (!std::regex_search(text, m, unitsExecRgx()))
        return false;
    number = AnimMath::toNumber(m[1].str());
    unit = m[2].str();
    return true;
}

void AnimValue::splitComplex(const std::string& text, std::vector<double>& numbers, std::vector<std::string>& strings){
    numbers.clear();
    strings.clear();
    size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), digitWithExponentRgx()); it != std::sregex_iterator(); ++it){
        const std::smatch& m = *it;
        numbers.push_back(AnimMath::toNumber(m.str()));
        strings.push_back(text.substr(last, (size_t)m.position() - last));
        last = (size_t)(m.position() + m.length());
    }
    strings.push_back(text.substr(last));
}

std::string AnimValue::toKebabCase(const std::string& name){
    std::string out;
    for (size_t i = 0; i < name.size(); i++){
        const char c = name[i];
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)name[i - 1]))
            out += '-';
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}
