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
    // locale (convert<float>::encode/decode), which costs more than the rest of the node.
    // These give the same text and the same values without the stream, for the floats
    // that make up most of a scene: Transforms, matrices, bounds, materials.

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
    bool readPlain(const std::string& text, T& value) {
        static_assert(std::is_same<T, float>::value || std::is_same<T, double>::value, "float or double");
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

    // node.as<T>() for a float or double, through readPlain when it can
    template <typename T>
    T as(const YAML::Node& node) {
        T value;
        if (node && node.IsScalar() && readPlain(node.Scalar(), value)) {
            return value;
        }
        return node.as<T>();
    }

}
