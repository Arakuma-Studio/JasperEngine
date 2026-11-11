// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ShaderGlobalsJson.h"

#include "util/Color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using Json = nlohmann::json;

namespace {

    // a float as JSON, without the noise of its double widening (0.1f -> 0.1, not 0.100000001...)
    // the shortest text that reads back as the same float
    Json shaderGlobalsJsonNumber(float value) {
        char text[32];
        for (int digits = 6; digits <= 9; digits++) {
            snprintf(text, sizeof(text), "%.*g", digits, value);
            if (std::strtof(text, nullptr) == value) break;
        }
        return std::strtod(text, nullptr);
    }

    Json numbers(const Vector4& value, int count) {
        Json list = Json::array();
        const float c[4] = {value.x, value.y, value.z, value.w};
        for (int i = 0; i < count; i++) list.push_back(shaderGlobalsJsonNumber(c[i]));
        return list;
    }

    bool readNumbers(const Json& json, int minCount, int maxCount, float* out, std::string& error, const std::string& what) {
        if (json.is_number() && minCount <= 1) {
            out[0] = json.get<float>();
            return true;
        }
        if (!json.is_array() || (int)json.size() < minCount || (int)json.size() > maxCount) {
            error = what + " must be " + (minCount == maxCount ? std::to_string(minCount) : std::to_string(minCount) + " to " + std::to_string(maxCount)) +
                    " numbers";
            return false;
        }
        for (size_t i = 0; i < json.size(); i++) {
            if (!json[i].is_number()) {
                error = what + " must be numbers";
                return false;
            }
            out[i] = json[i].get<float>();
            if (!std::isfinite(out[i])) {
                error = what + " must be finite";
                return false;
            }
        }
        return true;
    }

    int componentsOf(ShaderGlobalType type) {
        switch (type) {
            case ShaderGlobalType::Float:
            case ShaderGlobalType::FloatArray: return 1;
            case ShaderGlobalType::Vector2: return 2;
            case ShaderGlobalType::Vector3: return 3;
            default: return 4;
        }
    }

    // a colour: "#RRGGBB[AA]" or [r, g, b(, a)], sRGB 0..1, into linear
    bool readColor(const Json& json, Vector4& linear, std::string& error) {
        Vector4 srgb(0.0f, 0.0f, 0.0f, 1.0f);
        if (json.is_string()) {
            if (!ShaderGlobalsJson::parseHexColor(json.get<std::string>(), srgb)) {
                error = "a colour is \"#RRGGBB\", \"#RRGGBBAA\" or [r, g, b, a] (sRGB 0..1)";
                return false;
            }
        } else {
            float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (!readNumbers(json, 3, 4, c, error, "a colour")) {
                error = "a colour is \"#RRGGBB\", \"#RRGGBBAA\" or [r, g, b, a] (sRGB 0..1)";
                return false;
            }
            srgb = Vector4(c[0], c[1], c[2], c[3]);
        }
        // a negative component would be NaN through the gamma curve
        srgb = Vector4(std::max(0.0f, srgb.x), std::max(0.0f, srgb.y), std::max(0.0f, srgb.z), srgb.w);
        linear = Color::sRGBToLinear(srgb);
        return true;
    }

}

std::string ShaderGlobalsJson::exactHexColor(const Vector4& linear) {
    const Vector4 srgb = Color::linearTosRGB(linear);
    const float c[4] = {srgb.x, srgb.y, srgb.z, linear.w};
    int bytes[4];
    for (int i = 0; i < 4; i++) {
        if (!(c[i] >= -0.0001f && c[i] <= 1.0001f)) return "";
        bytes[i] = (int)std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f);
    }
    // only when reading the hex back gives the same linear value
    const Vector4 back = Color::sRGBToLinear(Vector4(bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f));
    if (back.x != linear.x || back.y != linear.y || back.z != linear.z || back.w != linear.w) return "";
    char text[16];
    snprintf(text, sizeof(text), "#%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3]);
    return text;
}

bool ShaderGlobalsJson::parseHexColor(const std::string& text, Vector4& srgb) {
    if (text.size() != 7 && text.size() != 9) return false;
    if (text[0] != '#') return false;
    for (size_t i = 1; i < text.size(); i++) {
        if (!std::isxdigit((unsigned char)text[i])) return false;
    }
    auto byteAt = [&text](size_t at) { return (float)std::stoi(text.substr(at, 2), nullptr, 16) / 255.0f; };
    srgb = Vector4(byteAt(1), byteAt(3), byteAt(5), text.size() == 9 ? byteAt(7) : 1.0f);
    return true;
}

Json ShaderGlobalsJson::valueToJson(ShaderGlobalType type, unsigned int arraySize, const std::vector<Vector4>& values) {
    auto slot = [&values](size_t i) { return i < values.size() ? values[i] : Vector4(0.0f, 0.0f, 0.0f, 0.0f); };
    switch (type) {
        case ShaderGlobalType::Float: return shaderGlobalsJsonNumber(slot(0).x);
        case ShaderGlobalType::Vector2:
        case ShaderGlobalType::Vector3:
        case ShaderGlobalType::Vector4: return numbers(slot(0), componentsOf(type));
        case ShaderGlobalType::Color: {
            const std::string hex = exactHexColor(slot(0));
            if (!hex.empty()) return hex;
            const Vector4 srgb = Color::linearTosRGB(slot(0));
            return numbers(Vector4(srgb.x, srgb.y, srgb.z, slot(0).w), 4);
        }
        case ShaderGlobalType::Matrix4: {
            Json list = Json::array();
            for (size_t c = 0; c < 4; c++) {
                for (const Json& n : numbers(slot(c), 4)) list.push_back(n);
            }
            return list;
        }
        case ShaderGlobalType::FloatArray: {
            Json list = Json::array();
            for (unsigned int i = 0; i < std::max(1u, arraySize); i++) list.push_back(shaderGlobalsJsonNumber(slot(i).x));
            return list;
        }
        case ShaderGlobalType::Vector4Array: {
            Json list = Json::array();
            for (unsigned int i = 0; i < std::max(1u, arraySize); i++) list.push_back(numbers(slot(i), 4));
            return list;
        }
        default: return "";
    }
}

bool ShaderGlobalsJson::valueFromJson(ShaderGlobalType type, unsigned int arraySize, const Json& json, std::vector<Vector4>& values, std::string& error) {
    values.clear();
    switch (type) {
        case ShaderGlobalType::Float:
        case ShaderGlobalType::Vector2:
        case ShaderGlobalType::Vector3:
        case ShaderGlobalType::Vector4: {
            const int count = componentsOf(type);
            float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            // missing components are 0
            if (!readNumbers(json, 1, count, c, error, std::string("a ") + ShaderGlobals::getTypeName(type)))
                return false;
            values.push_back(Vector4(c[0], c[1], c[2], c[3]));
            return true;
        }
        case ShaderGlobalType::Color: {
            Vector4 linear;
            if (!readColor(json, linear, error)) return false;
            values.push_back(linear);
            return true;
        }
        case ShaderGlobalType::Matrix4: {
            float m[16];
            if (!readNumbers(json, 16, 16, m, error, "a matrix4 (column-major)")) return false;
            for (int c = 0; c < 4; c++) values.push_back(Vector4(m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]));
            return true;
        }
        case ShaderGlobalType::FloatArray:
        case ShaderGlobalType::Vector4Array: {
            const bool vectors = type == ShaderGlobalType::Vector4Array;
            if (!json.is_array() || json.empty() || json.size() > std::max(1u, arraySize)) {
                error = std::string("a ") + ShaderGlobals::getTypeName(type) + " default is a list of 1 to " +
                        std::to_string(std::max(1u, arraySize)) + (vectors ? " [x, y, z, w] lists" : " numbers") + " (array_size)";
                return false;
            }
            for (size_t i = 0; i < json.size(); i++) {
                float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!readNumbers(json[i], 1, vectors ? 4 : 1, c, error, "element " + std::to_string(i)))
                    return false;
                values.push_back(Vector4(c[0], vectors ? c[1] : 0.0f, vectors ? c[2] : 0.0f, vectors ? c[3] : 0.0f));
            }
            return true;
        }
        default:
            error = "a texture global's default is an asset path";
            return false;
    }
}

Json ShaderGlobalsJson::declToJson(const ShaderGlobalDecl& decl, const char* arraySizeKey) {
    Json json = Json::object();
    json["name"] = decl.name;
    json["type"] = ShaderGlobals::getTypeName(decl.type);
    if (ShaderGlobals::isArray(decl.type)) json[arraySizeKey] = decl.arraySize;
    if (ShaderGlobals::isTexture(decl.type))
        json["default"] = decl.defaultTexture;
    else
        json["default"] = valueToJson(decl.type, decl.arraySize, decl.defaultValue);
    return json;
}

bool ShaderGlobalsJson::declFromJson(const Json& json, ShaderGlobalDecl& out, std::string& error, const std::string& nameIfMissing,
                                     const ShaderGlobalDecl* base) {
    if (!json.is_object()) {
        error = "a shader global is {name, type, default, array_size}";
        return false;
    }
    ShaderGlobalDecl decl;
    decl.name = json.contains("name") && json["name"].is_string() ? json["name"].get<std::string>() : nameIfMissing;
    if (!ShaderGlobals::isValidName(decl.name, &error)) return false;

    if (json.contains("type")) {
        if (!json["type"].is_string() || !ShaderGlobals::parseTypeName(json["type"].get<std::string>(), decl.type)) {
            error = "'" + decl.name + "': type is one of float, vector2, vector3, vector4, color, matrix4, float_array, "
                    "vector4_array, texture2d, texturecube";
            return false;
        }
    } else if (base) {
        decl.type = base->type;
    } else {
        error = "'" + decl.name + "' needs a type (float, vector2, vector3, vector4, color, matrix4, float_array, "
                "vector4_array, texture2d, texturecube)";
        return false;
    }

    const char* sizeKeys[] = {"array_size", "arraySize"};
    bool sized = false;
    for (const char* key : sizeKeys) {
        if (!json.contains(key)) continue;
        if (!json[key].is_number_integer() || json[key].get<int>() < 1 || json[key].get<int>() > (int)ShaderGlobals::MAX_ARRAY_SIZE) {
            error = "'" + decl.name + "': array_size is 1 to " + std::to_string(ShaderGlobals::MAX_ARRAY_SIZE);
            return false;
        }
        decl.arraySize = (unsigned int)json[key].get<int>();
        sized = true;
    }
    if (ShaderGlobals::isArray(decl.type)) {
        if (!sized) {
            if (base && base->type == decl.type) {
                decl.arraySize = base->arraySize;
            } else if (json.contains("default") && json["default"].is_array() && !json["default"].empty()) {
                decl.arraySize = (unsigned int)std::min<size_t>(json["default"].size(), ShaderGlobals::MAX_ARRAY_SIZE);
            } else {
                error = "'" + decl.name + "': an array needs array_size (1 to " + std::to_string(ShaderGlobals::MAX_ARRAY_SIZE) + ")";
                return false;
            }
        }
    } else {
        decl.arraySize = 0;
    }

    const ShaderGlobalDecl typed = ShaderGlobals::makeDecl(decl.name, decl.type, decl.arraySize);
    decl.defaultValue = typed.defaultValue;
    if (json.contains("default") && !json["default"].is_null()) {
        const Json& value = json["default"];
        if (ShaderGlobals::isTexture(decl.type)) {
            if (!value.is_string()) {
                error = "'" + decl.name + "': a texture global's default is an asset path (\"\" for none)";
                return false;
            }
            decl.defaultTexture = value.get<std::string>();
        } else {
            std::vector<Vector4> values;
            if (!valueFromJson(decl.type, decl.arraySize, value, values, error)) {
                error = "'" + decl.name + "': " + error;
                return false;
            }
            for (size_t i = 0; i < values.size() && i < decl.defaultValue.size(); i++) decl.defaultValue[i] = values[i];
        }
    } else if (base && base->type == decl.type && base->arraySize == decl.arraySize) {
        // no default given: the same type and size keep the current one (another type takes its own)
        decl.defaultValue = base->defaultValue;
        decl.defaultTexture = base->defaultTexture;
    }
    out = decl;
    return true;
}

Json ShaderGlobalsJson::listToJson(const std::vector<ShaderGlobalDecl>& decls, const char* arraySizeKey) {
    Json list = Json::array();
    for (const ShaderGlobalDecl& decl : decls) list.push_back(declToJson(decl, arraySizeKey));
    return list;
}

bool ShaderGlobalsJson::listFromJson(const Json& json, std::vector<ShaderGlobalDecl>& out, std::string& error) {
    if (!json.is_array()) {
        error = "must be a list [{name, type, default, array_size}, ...]";
        return false;
    }
    std::vector<ShaderGlobalDecl> decls;
    for (const Json& item : json) {
        ShaderGlobalDecl decl;
        if (!declFromJson(item, decl, error)) return false;
        decls.push_back(decl);
    }
    error = ShaderGlobals::validate(decls);
    if (!error.empty()) return false;
    out = decls;
    return true;
}
