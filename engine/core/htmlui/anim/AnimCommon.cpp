// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimCommon.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;

double AnimMath::jsRound(double v){
    if (!std::isfinite(v))
        return v;
    double r = std::floor(v);
    if (v - r >= 0.5)
        r += 1.0;
    // Math.round(-0.4) is -0
    if (r == 0.0 && std::signbit(v))
        return -0.0;
    return r;
}

double AnimMath::round(double v, int decimals){
    if (decimals < 0)
        return v;
    if (decimals == 0)
        return jsRound(v);
    const double p = std::pow(10.0, decimals);
    return jsRound(v * p) / p;
}

namespace {

    bool isJsSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    // length of the StrDecimalLiteral at s (sign, digits, fraction, exponent), 0 when none;
    // Infinity is accepted like JavaScript
    size_t decimalLiteralLength(const char* s){
        size_t i = 0;
        if (s[i] == '+' || s[i] == '-')
            i++;
        if (std::strncmp(s + i, "Infinity", 8) == 0)
            return i + 8;
        size_t digits = 0;
        while (std::isdigit((unsigned char)s[i])){ i++; digits++; }
        if (s[i] == '.'){
            i++;
            while (std::isdigit((unsigned char)s[i])){ i++; digits++; }
        }
        if (digits == 0)
            return 0;
        if (s[i] == 'e' || s[i] == 'E'){
            size_t j = i + 1;
            if (s[j] == '+' || s[j] == '-')
                j++;
            if (std::isdigit((unsigned char)s[j])){
                while (std::isdigit((unsigned char)s[j])) j++;
                i = j;
            }
        }
        return i;
    }

    double parseLiteral(const char* s, size_t length){
        std::string literal(s, length);
        const char* text = literal.c_str();
        const char* inf = std::strstr(text, "Infinity");
        if (inf)
            return text[0] == '-' ? -AnimConst::infinity : AnimConst::infinity;
        return std::strtod(text, nullptr);
    }

}

double AnimMath::toNumber(const std::string& text){
    size_t begin = 0, end = text.size();
    while (begin < end && isJsSpace(text[begin])) begin++;
    while (end > begin && isJsSpace(text[end - 1])) end--;
    if (begin == end)
        return 0.0;
    std::string trimmed = text.substr(begin, end - begin);
    // hexadecimal integers ("0x1f")
    if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')){
        double v = 0.0;
        for (size_t i = 2; i < trimmed.size(); i++){
            char c = (char)std::tolower((unsigned char)trimmed[i]);
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0)
                return std::nan("");
            v = v * 16.0 + digit;
        }
        return v;
    }
    size_t length = decimalLiteralLength(trimmed.c_str());
    if (length != trimmed.size())
        return std::nan("");
    return parseLiteral(trimmed.c_str(), length);
}

double AnimMath::parseFloat(const std::string& text){
    size_t begin = 0;
    while (begin < text.size() && isJsSpace(text[begin])) begin++;
    const char* s = text.c_str() + begin;
    size_t length = decimalLiteralLength(s);
    if (length == 0)
        return std::nan("");
    return parseLiteral(s, length);
}

std::string AnimMath::toString(double v){
    if (std::isnan(v))
        return "NaN";
    if (std::isinf(v))
        return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0.0)
        return "0";

    std::string sign = v < 0 ? "-" : "";
    double a = std::fabs(v);

    // shortest digits that round trip
    char buffer[64];
    int precision = 1;
    for (; precision <= 17; precision++){
        std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, a);
        if (std::strtod(buffer, nullptr) == a)
            break;
    }
    // buffer: d.ddddde[+-]xx
    std::string digits;
    const char* p = buffer;
    for (; *p && *p != 'e'; p++){
        if (std::isdigit((unsigned char)*p))
            digits += *p;
    }
    int exponent = std::atoi(p + 1);
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();

    // ECMAScript Number::toString: k digits, n = exponent + 1
    const int k = (int)digits.size();
    const int n = exponent + 1;
    std::string out;
    if (k <= n && n <= 21){
        out = digits + std::string(n - k, '0');
    } else if (0 < n && n <= 21){
        out = digits.substr(0, n) + "." + digits.substr(n);
    } else if (-6 < n && n <= 0){
        out = "0." + std::string(-n, '0') + digits;
    } else {
        std::string e = (n - 1 >= 0 ? "+" : "-") + std::to_string(std::abs(n - 1));
        out = digits.substr(0, 1) + (k > 1 ? "." + digits.substr(1) : "") + "e" + e;
    }
    return sign + out;
}
