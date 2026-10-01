// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "yaml-cpp/yaml.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>

namespace jasperengine::editor::YamlNumbers {

    // yaml-cpp converts every number through a std::stringstream imbued with the classic
    // locale (convert<T>::encode/decode), which costs more than the rest of the node.
    // These give the same text and the same values without the stream, for the numbers
    // and flags that make up most of a scene: Transforms, matrices, bounds, materials.
    // Anything they do not handle exactly is left to yaml-cpp.

    // The scalar text `YAML::Node(value)` holds: yaml-cpp writes a float as FpToString at
    // max_digits10, and NaN / infinities as .nan / .inf / -.inf. Assigned to a map value or
    // a sequence element (node[i] = text), the scalar is made in that node's tree, where
    // YAML::Node(value) builds a tree of its own that is then merged in.
    template <typename T>
    std::string text(T value) {
        static_assert(std::is_same<T, float>::value || std::is_same<T, double>::value, "float or double");
        if (std::isnan(value)) {
            return ".nan";
        }
        if (std::isinf(value)) {
            return std::signbit(value) ? "-.inf" : ".inf";
        }
        return YAML::FpToString(value, std::numeric_limits<T>::max_digits10);
    }

    // The scalar text of `YAML::Node(value)` for a bool (convert<bool>::encode)
    inline const char* text(bool value) {
        return value ? "true" : "false";
    }

    // Reads [-]digits[.digits][(e|E)[+|-]digits] as `node.as<T>()` reads it: the stream
    // collects such text whole and hands it to strtof/strtod. False for any other text,
    // for text strtof/strtod do not read whole (a locale whose decimal point is not '.'),
    // and for an overflow (where as<T>() throws): the caller then takes the yaml-cpp path.
    template <typename T>
    typename std::enable_if<std::is_same<T, float>::value || std::is_same<T, double>::value, bool>::type
    readPlain(const std::string& text, T& value) {
        const char* begin = text.c_str();
        const char* end = begin + text.size();
        const char* c = begin;
        auto digits = [&c, end]() {
            const char* first = c;
            while (c != end && *c >= '0' && *c <= '9') {
                ++c;
            }
            return c != first;
        };
        if (c != end && *c == '-') {
            ++c;
        }
        if (!digits()) {
            return false;
        }
        if (c != end && *c == '.') {
            ++c;
            if (!digits()) {
                return false;
            }
        }
        if (c != end && (*c == 'e' || *c == 'E')) {
            ++c;
            if (c != end && (*c == '+' || *c == '-')) {
                ++c;
            }
            if (!digits()) {
                return false;
            }
        }
        if (c != end) {
            return false;
        }
        char* parsed = nullptr;
        const T result = std::is_same<T, float>::value ?
            static_cast<T>(std::strtof(begin, &parsed)) : static_cast<T>(std::strtod(begin, &parsed));
        if (parsed != end || std::isinf(result)) {
            return false;
        }
        value = result;
        return true;
    }

    template <typename T>
    struct IsPlainInteger : std::integral_constant<bool, std::is_integral<T>::value && !std::is_same<T, bool>::value &&
        !std::is_same<T, char>::value && !std::is_same<T, signed char>::value && !std::is_same<T, unsigned char>::value> {};

    // Reads [-]digits in base 10 as `node.as<T>()` reads it into an integer. The stream takes
    // "0x1f" as hex and "017" as octal, so a number with a leading zero (other than 0 itself)
    // is left to it, as is one of more than 19 digits, a sign on an unsigned type, and a
    // value out of the type's range (where as<T>() throws).
    template <typename T>
    typename std::enable_if<IsPlainInteger<T>::value, bool>::type
    readPlain(const std::string& text, T& value) {
        const char* c = text.data();
        const char* end = c + text.size();
        const bool negative = c != end && *c == '-';
        if (negative) {
            if (std::is_unsigned<T>::value) {
                return false;
            }
            ++c;
        }
        if (c == end || end - c > 19 || (*c == '0' && end - c > 1)) {
            return false;
        }
        unsigned long long magnitude = 0;
        for (; c != end; ++c) {
            if (*c < '0' || *c > '9') {
                return false;
            }
            magnitude = magnitude * 10 + static_cast<unsigned long long>(*c - '0');
        }
        if (negative) {
            // |min| of T, computed without overflowing it
            const unsigned long long limit = static_cast<unsigned long long>(-((std::numeric_limits<T>::min)() + 1)) + 1;
            if (magnitude > limit) {
                return false;
            }
            value = magnitude == limit ? (std::numeric_limits<T>::min)() : static_cast<T>(-static_cast<long long>(magnitude));
            return true;
        }
        if (magnitude > static_cast<unsigned long long>((std::numeric_limits<T>::max)())) {
            return false;
        }
        value = static_cast<T>(magnitude);
        return true;
    }

    // What convert<bool>::decode reads from "true" and "false", the text yaml-cpp writes;
    // other spellings (yes, on, True...) are left to it
    inline bool readPlain(const std::string& text, bool& value) {
        if (text == "true") {
            value = true;
            return true;
        }
        if (text == "false") {
            value = false;
            return true;
        }
        return false;
    }

    // The scalar text of `YAML::Node(value)` for an integer: the classic-locale stream prints
    // what std::to_string does
    template <typename T>
    typename std::enable_if<IsPlainInteger<T>::value, std::string>::type
    integerText(T value) {
        return std::to_string(value);
    }

    // node.as<T>() for a float, an integer or a bool, through readPlain when it can
    template <typename T>
    T as(const YAML::Node& node) {
        T value;
        if (node && node.IsScalar() && readPlain(node.Scalar(), value)) {
            return value;
        }
        return node.as<T>();
    }

}
