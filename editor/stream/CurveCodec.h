// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Curves and gradients in the editor's three encodings (docs/specs/S27-editor-tools.md R45, D4),
// all the one JSON form of engine/core/math/CurveJson.h: scene YAML (the same structure, key
// arrays in flow style), MCP JSON (identical to the file form), and the exported code's C++
// literal. Min-max values use the particle effect JSON (ParticleEffectCodec's field form).

#include "math/Curve.h"
#include "mcp/McpJson.h"
#include "particles/ParticleValues.h"

#include "yaml-cpp/yaml.h"

#include <string>

namespace jasperengine::editor::curvecodec {

    YAML::Node curveToYaml(const Curve& curve);
    // false with `reason` (its path first, "keys[2]: ...") when the node holds no curve
    bool curveFromYaml(const YAML::Node& node, Curve& curve, std::string& reason);

    mcp::Json curveToJson(const Curve& curve);
    bool curveFromJson(const mcp::Json& json, Curve& curve, std::string& reason);

    YAML::Node gradientToYaml(const Gradient& gradient);
    bool gradientFromYaml(const YAML::Node& node, Gradient& gradient, std::string& reason);
    mcp::Json gradientToJson(const Gradient& gradient);
    bool gradientFromJson(const mcp::Json& json, Gradient& gradient, std::string& reason);

    // {mode, ...} with the members the mode uses (the particle effect field form)
    mcp::Json minMaxCurveToJson(const particles::MinMaxCurve& value);
    bool minMaxCurveFromJson(const mcp::Json& json, particles::MinMaxCurve& value, std::string& reason);
    mcp::Json minMaxGradientToJson(const particles::MinMaxGradient& value);
    bool minMaxGradientFromJson(const mcp::Json& json, particles::MinMaxGradient& value, std::string& reason);

    // The exported code's expression of a curve:
    // Curve({CurveKey{0.0f, 0.0f, 1.0f, 1.0f, 0}, ...}) with the wraps set by a lambda when not clamp
    std::string curveCppLiteral(const Curve& curve);
    // ... of a gradient: Gradient::twoColors / constant when it is one, else a lambda filling
    // its keys (sRGB-encoded colours), mode and linear flag
    std::string gradientCppLiteral(const Gradient& gradient);

    // The JSON Schema of a curve (MCP tool and property schemas)
    mcp::Json curveSchema();
    mcp::Json gradientSchema();

}
