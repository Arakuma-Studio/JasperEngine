// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NumberText.h"

#include <cctype>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace {

    // The decimal point printf and strtod use now (LC_NUMERIC)
    std::string localeDecimalPoint(){
        const std::lconv* conv = std::localeconv();
        if (!conv || !conv->decimal_point || !conv->decimal_point[0])
            return ".";
        return conv->decimal_point;
    }

    std::string printed(double value, int precision, const std::string& point){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        std::string text = buffer;
        if (point != "."){
            const size_t at = text.find(point);
            if (at != std::string::npos)
                text.replace(at, point.size(), ".");
        }
        return text;
    }

    // The decimal exponent of `value` printed with `precision` significant digits
    int exponentOf(double value, int precision){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, value);
        const char* e = std::strchr(buffer, 'e');
        return e ? std::atoi(e + 1) : 0;
    }

    // `shortest` digits are enough to read back; %g with them may still pick the exponent form
    // ("1e+02" for 100), so fixed notation gets the digits up to the decimal point too
    std::string readable(double value, int shortest, int maxPrecision, const std::string& point){
        const int exponent = exponentOf(value, shortest);
        int precision = shortest;
        if (exponent >= shortest && exponent < maxPrecision)
            precision = exponent + 1;
        return printed(value, precision, point);
    }

    const char* nonFinite(double value){
        if (std::isnan(value)) return "nan";
        return value > 0 ? "inf" : "-inf";
    }

    bool equalsIgnoreCase(const std::string& text, size_t from, const char* word){
        size_t i = from;
        for (const char* c = word; *c; ++c, ++i){
            if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != *c)
                return false;
        }
        return i == text.size();
    }

    // [+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?
    bool isDecimalNumber(const std::string& text){
        size_t i = 0;
        const size_t n = text.size();
        if (i < n && (text[i] == '+' || text[i] == '-')) i++;
        size_t digits = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; digits++; }
        if (i < n && text[i] == '.'){
            i++;
            while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; digits++; }
        }
        if (digits == 0) return false;
        if (i < n && (text[i] == 'e' || text[i] == 'E')){
            i++;
            if (i < n && (text[i] == '+' || text[i] == '-')) i++;
            size_t exponent = 0;
            while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; exponent++; }
            if (exponent == 0) return false;
        }
        return i == n;
    }

}

std::string NumberText::formatFloat(float value){
    if (!std::isfinite(value)) return nonFinite(value);
    const std::string point = localeDecimalPoint();
    int precision = 1;
    for (; precision < 9; precision++){
        float back = 0.0f;
        if (parseFloat(printed(static_cast<double>(value), precision, point), back) && back == value) break;
    }
    return readable(static_cast<double>(value), precision, 9, point);
}

std::string NumberText::formatDouble(double value){
    if (!std::isfinite(value)) return nonFinite(value);
    const std::string point = localeDecimalPoint();
    int precision = 1;
    for (; precision < 17; precision++){
        double back = 0.0;
        if (parseDouble(printed(value, precision, point), back) && back == value) break;
    }
    return readable(value, precision, 17, point);
}

double NumberText::floatToDouble(float value){
    if (!std::isfinite(value)) return static_cast<double>(value);
    double out = static_cast<double>(value);
    parseDouble(formatFloat(value), out);
    return out;
}

bool NumberText::parseDouble(const std::string& text, double& out){
    size_t start = 0;
    bool negative = false;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')){
        negative = text[0] == '-';
        start = 1;
    }
    if (equalsIgnoreCase(text, start, "nan")){
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (equalsIgnoreCase(text, start, "inf") || equalsIgnoreCase(text, start, "infinity")){
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (!isDecimalNumber(text)) return false;

    std::string local = text;
    const std::string point = localeDecimalPoint();
    if (point != "."){
        const size_t at = local.find('.');
        if (at != std::string::npos)
            local.replace(at, 1, point);
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(local.c_str(), &end);
    if (end != local.c_str() + local.size()) return false;
    // overflow (1e400); an underflow gives the nearest denormal or zero, which is fine
    if (errno == ERANGE && std::isinf(value)) return false;
    out = value;
    return true;
}

bool NumberText::parseFloat(const std::string& text, float& out){
    double value = 0.0;
    if (!parseDouble(text, value)) return false;
    const double max = static_cast<double>(std::numeric_limits<float>::max());
    if (std::isfinite(value) && std::fabs(value) > max){
        // what rounds to the largest float is that float; beyond half a step it is out of range
        if (std::fabs(value) >= max + std::ldexp(1.0, 103)) return false;
        value = std::copysign(max, value);
    }
    out = static_cast<float>(value);
    return true;
}

bool NumberText::parseInteger(const std::string& text, long long& out){
    size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
    if (i == text.size()) return false;
    for (size_t k = i; k < text.size(); k++){
        if (!std::isdigit(static_cast<unsigned char>(text[k]))) return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size()) return false;
    out = value;
    return true;
}
