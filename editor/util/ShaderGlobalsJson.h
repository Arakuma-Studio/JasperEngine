// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "shader/ShaderGlobals.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // Project Settings > Graphics > Shader Globals as JSON, the one shape project.yaml (through
    // YAML) and MCP (get/set_project_settings graphics.shader_globals) share:
    //   {name, type, array_size (arrays), default}
    // default by type: float a number; vector2..4 a list of 2..4 numbers; color "#RRGGBB[AA]"
    // (sRGB; written as hex when the value is an 8-bit colour, else [r, g, b, a] sRGB numbers);
    // matrix4 16 numbers, column-major; float_array N numbers; vector4_array N lists of 4;
    // texture2d / texturecube an asset path ("" = white / black).
    namespace ShaderGlobalsJson {
        // arraySizeKey: "array_size" (MCP) or "arraySize" (project.yaml)
        nlohmann::json declToJson(const ShaderGlobalDecl& decl, const char* arraySizeKey = "array_size");
        // the name from `name` (else nameIfMissing); the type defaults to the current one when
        // `type` is absent and base is given (an upsert of the default only). False with error
        bool declFromJson(const nlohmann::json& json, ShaderGlobalDecl& out, std::string& error,
                          const std::string& nameIfMissing = "", const ShaderGlobalDecl* base = nullptr);
        nlohmann::json listToJson(const std::vector<ShaderGlobalDecl>& decls, const char* arraySizeKey = "array_size");
        // the whole list, validated (names, duplicates, caps) like ShaderGlobals::validate
        bool listFromJson(const nlohmann::json& json, std::vector<ShaderGlobalDecl>& out, std::string& error);

        // a value of the type (linear slots as ShaderGlobalDecl::defaultValue) as JSON
        nlohmann::json valueToJson(ShaderGlobalType type, unsigned int arraySize, const std::vector<Vector4>& values);
        bool valueFromJson(ShaderGlobalType type, unsigned int arraySize, const nlohmann::json& json,
                           std::vector<Vector4>& values, std::string& error);
        // "#RRGGBBAA" of a linear colour when it is exactly an 8-bit sRGB colour, else ""
        std::string exactHexColor(const Vector4& linear);
        bool parseHexColor(const std::string& text, Vector4& srgb);
    }

}
