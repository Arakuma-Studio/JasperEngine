// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveCodec.h"

#include "math/CurveJson.h"
#include "particles/ParticleEffectCodec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using Ordered = nlohmann::ordered_json;

    // the shortest decimal that reads back as the same float ("0.1", "1e-05")
    std::string shortestText(float value) {
        char text[32];
        for (int precision = 1; precision <= 9; precision++) {
            std::snprintf(text, sizeof(text), "%.*g", precision, (double)value);
            if ((float)std::strtod(text, nullptr) == value) return text;
        }
        std::snprintf(text, sizeof(text), "%.9g", (double)value);
        return text;
    }

    // the ordered JSON as YAML: arrays of scalars (a key, a colour) in flow style
    YAML::Node toYaml(const Ordered& j) {
        if (j.is_object()) {
            YAML::Node node(YAML::NodeType::Map);
            for (auto it = j.begin(); it != j.end(); ++it) node[it.key()] = toYaml(*it);
            return node;
        }
        if (j.is_array()) {
            YAML::Node node(YAML::NodeType::Sequence);
            bool scalars = true;
            for (const Ordered& element : j) {
                node.push_back(toYaml(element));
                scalars = scalars && !element.is_array() && !element.is_object();
            }
            if (scalars) node.SetStyle(YAML::EmitterStyle::Flow);
            return node;
        }
        if (j.is_null()) return YAML::Node(YAML::NodeType::Null);
        if (j.is_boolean()) return YAML::Node(j.get<bool>());
        if (j.is_string()) return YAML::Node(j.get<std::string>());
        // a number: the float's shortest round-trip text
        return YAML::Node(shortestText(j.get<float>()));
    }

    bool fromYaml(const YAML::Node& node, Ordered& out, std::string& reason) {
        switch (node.Type()) {
            case YAML::NodeType::Map:
                out = Ordered::object();
                for (auto it = node.begin(); it != node.end(); ++it) {
                    Ordered value;
                    if (!fromYaml(it->second, value, reason)) return false;
                    out[it->first.as<std::string>()] = std::move(value);
                }
                return true;
            case YAML::NodeType::Sequence:
                out = Ordered::array();
                for (const YAML::Node& element : node) {
                    Ordered value;
                    if (!fromYaml(element, value, reason)) return false;
                    out.push_back(std::move(value));
                }
                return true;
            case YAML::NodeType::Null:
                out = nullptr;
                return true;
            case YAML::NodeType::Scalar: {
                const std::string text = node.Scalar();
                if (text == "true" || text == "false") {
                    out = text == "true";
                    return true;
                }
                // YAML's infinities (older force field files wrote step tangents .inf): a step
                if (text == ".inf" || text == ".Inf" || text == "+.inf" || text == "-.inf" || text == "-.Inf" ||
                    text == ".nan" || text == ".NaN") {
                    out = nullptr;
                    return true;
                }
                // a key's mode token is the only string of the form; everything else is a number
                char* end = nullptr;
                const double number = std::strtod(text.c_str(), &end);
                if (!text.empty() && end && *end == '\0') {
                    out = number;
                } else {
                    out = text;
                }
                return true;
            }
            case YAML::NodeType::Undefined:
                break;
        }
        reason = "no value";
        return false;
    }

    std::string issueText(const std::vector<curvejson::Issue>& issues) {
        for (const curvejson::Issue& issue : issues) {
            if (issue.error) {
                std::string path = issue.path;
                if (!path.empty() && path[0] == '.') path.erase(0, 1);
                return path.empty() ? issue.message : path + ": " + issue.message;
            }
        }
        return std::string();
    }

    std::string diagnosticText(const std::vector<particles::Diagnostic>& diagnostics) {
        for (const particles::Diagnostic& d : diagnostics) {
            if (d.severity == particles::Diagnostic::Severity::Error) {
                return d.path.empty() || d.path == "$" ? d.message : d.path + ": " + d.message;
            }
        }
        return std::string();
    }

    // a C++ float literal that reads back as the same float ("0.5f", "1.0f", "1e-05f")
    std::string floatLiteral(float value) {
        if (std::isinf(value)) return value > 0 ? "INFINITY" : "-INFINITY";
        if (std::isnan(value)) return "0.0f";
        std::string text = shortestText(value);
        if (text.find_first_of(".e") == std::string::npos) text += ".0";
        return text + "f";
    }

    const char* wrapLiteral(CurveWrapMode mode) {
        switch (mode) {
            case CurveWrapMode::Clamp: return "CurveWrapMode::Clamp";
            case CurveWrapMode::Loop: return "CurveWrapMode::Loop";
            case CurveWrapMode::PingPong: return "CurveWrapMode::PingPong";
            case CurveWrapMode::Linear: return "CurveWrapMode::Linear";
            case CurveWrapMode::LoopWithOffset: return "CurveWrapMode::LoopWithOffset";
        }
        return "CurveWrapMode::Clamp";
    }

}

YAML::Node curvecodec::curveToYaml(const Curve& curve) {
    return toYaml(curvejson::curveToJson(curve));
}

bool curvecodec::curveFromYaml(const YAML::Node& node, Curve& curve, std::string& reason) {
    Ordered j;
    if (!fromYaml(node, j, reason)) return false;
    std::vector<curvejson::Issue> issues;
    Curve read;
    if (!curvejson::curveFromJson(j, read, issues)) {
        reason = issueText(issues);
        return false;
    }
    curve = std::move(read);
    return true;
}

mcp::Json curvecodec::curveToJson(const Curve& curve) {
    return mcp::Json::parse(curvejson::curveToJson(curve).dump());
}

namespace {

    // A value from MCP (not a file, read leniently): an unknown key is refused in either form,
    // and in origin/main's form whatever its reader refused (a time out of 0..1, a wrap name)
    bool refuseUpstreamWarnings(bool upstream, const std::vector<curvejson::Issue>& issues, std::string& reason) {
        for (const curvejson::Issue& issue : issues) {
            if (!upstream && issue.message.find("unknown") == std::string::npos) continue;
            std::string path = issue.path;
            if (!path.empty() && path[0] == '.') path.erase(0, 1);
            reason = path.empty() ? issue.message : path + ": " + issue.message;
            return true;
        }
        return false;
    }

}

bool curvecodec::curveFromJson(const mcp::Json& json, Curve& curve, std::string& reason) {
    std::vector<curvejson::Issue> issues;
    Curve read;
    const Ordered j = Ordered::parse(json.dump());
    if (!curvejson::curveFromJson(j, read, issues)) {
        reason = issueText(issues);
        return false;
    }
    if (refuseUpstreamWarnings(curvejson::isUpstreamCurve(j), issues, reason)) return false;
    curve = std::move(read);
    return true;
}

YAML::Node curvecodec::gradientToYaml(const Gradient& gradient) {
    return toYaml(curvejson::gradientToJson(gradient));
}

bool curvecodec::gradientFromYaml(const YAML::Node& node, Gradient& gradient, std::string& reason) {
    Ordered j;
    if (!fromYaml(node, j, reason)) return false;
    std::vector<curvejson::Issue> issues;
    Gradient read;
    if (!curvejson::gradientFromJson(j, read, issues)) {
        reason = issueText(issues);
        return false;
    }
    gradient = std::move(read);
    return true;
}

mcp::Json curvecodec::gradientToJson(const Gradient& gradient) {
    return mcp::Json::parse(curvejson::gradientToJson(gradient).dump());
}

bool curvecodec::gradientFromJson(const mcp::Json& json, Gradient& gradient, std::string& reason) {
    std::vector<curvejson::Issue> issues;
    Gradient read;
    const Ordered j = Ordered::parse(json.dump());
    if (!curvejson::gradientFromJson(j, read, issues)) {
        reason = issueText(issues);
        return false;
    }
    if (refuseUpstreamWarnings(curvejson::isUpstreamGradient(j), issues, reason)) return false;
    gradient = std::move(read);
    return true;
}

mcp::Json curvecodec::minMaxCurveToJson(const particles::MinMaxCurve& value) {
    return mcp::Json::parse(particles::ParticleEffectCodec::minMaxCurveToJson(value).dump());
}

bool curvecodec::minMaxCurveFromJson(const mcp::Json& json, particles::MinMaxCurve& value, std::string& reason) {
    std::vector<particles::Diagnostic> diagnostics;
    particles::MinMaxCurve read;
    if (!particles::ParticleEffectCodec::minMaxCurveFromJson(Ordered::parse(json.dump()), read, diagnostics)) {
        reason = diagnosticText(diagnostics);
        return false;
    }
    value = std::move(read);
    return true;
}

mcp::Json curvecodec::minMaxGradientToJson(const particles::MinMaxGradient& value) {
    return mcp::Json::parse(particles::ParticleEffectCodec::minMaxGradientToJson(value).dump());
}

bool curvecodec::minMaxGradientFromJson(const mcp::Json& json, particles::MinMaxGradient& value, std::string& reason) {
    std::vector<particles::Diagnostic> diagnostics;
    particles::MinMaxGradient read;
    if (!particles::ParticleEffectCodec::minMaxGradientFromJson(Ordered::parse(json.dump()), read, diagnostics)) {
        reason = diagnosticText(diagnostics);
        return false;
    }
    value = std::move(read);
    return true;
}

std::string curvecodec::curveCppLiteral(const Curve& curve) {
    std::string keys = "Curve({";
    for (size_t i = 0; i < curve.keys.size(); i++) {
        const CurveKey& k = curve.keys[i];
        if (i > 0) keys += ", ";
        keys += "CurveKey{" + floatLiteral(k.time) + ", " + floatLiteral(k.value) + ", " + floatLiteral(k.inTangent) + ", " +
                floatLiteral(k.outTangent) + ", " + std::to_string((int)k.mode) + "}";
    }
    keys += "})";
    if (curve.preWrapMode == CurveWrapMode::Clamp && curve.postWrapMode == CurveWrapMode::Clamp) return keys;
    return std::string("[]{ Curve c = ") + keys + "; c.preWrapMode = " + wrapLiteral(curve.preWrapMode) +
           "; c.postWrapMode = " + wrapLiteral(curve.postWrapMode) + "; return c; }()";
}

std::string curvecodec::gradientCppLiteral(const Gradient& gradient) {
    auto vector3 = [](const Vector3& v) {
        return "Vector3(" + floatLiteral(v.x) + ", " + floatLiteral(v.y) + ", " + floatLiteral(v.z) + ")";
    };
    std::string code = "[]{ Gradient g; g.colorKeys = {";
    for (size_t i = 0; i < gradient.colorKeys.size(); i++) {
        if (i > 0) code += ", ";
        code += "GradientColorKey{" + floatLiteral(gradient.colorKeys[i].time) + ", " + vector3(gradient.colorKeys[i].color) + "}";
    }
    code += "}; g.alphaKeys = {";
    for (size_t i = 0; i < gradient.alphaKeys.size(); i++) {
        if (i > 0) code += ", ";
        code += "GradientAlphaKey{" + floatLiteral(gradient.alphaKeys[i].time) + ", " + floatLiteral(gradient.alphaKeys[i].alpha) + "}";
    }
    code += "};";
    if (gradient.mode == GradientMode::Fixed) code += " g.mode = GradientMode::Fixed;";
    if (gradient.linear) code += " g.linear = true;";
    return code + " return g; }()";
}

mcp::Json curvecodec::curveSchema() {
    const mcp::Json key = {{"type", "array"}, {"minItems", 2}, {"maxItems", 5},
                           {"description", "[time, value, inTangent, outTangent, mode?]; null (or \"inf\") tangent = step"}};
    return {{"description", "A curve: [[time, value, inTangent, outTangent(, mode)], ...] or {keys, preWrapMode, postWrapMode} "
                            "(wraps clamp|loop|pingPong|linear|loopWithOffset; modes clampedAuto|auto|free|linear|constant or "
                            "in:<m>,out:<m>[,broken]); also a number (a constant curve) and {keys, pre, post} (wraps "
                            "clamp|loop|ping_pong)"},
            {"anyOf", mcp::Json::array({{{"type", "number"}},
                                        {{"type", "array"}, {"items", key}},
                                        {{"type", "object"}, {"properties", {{"keys", {{"type", "array"}, {"items", key}}},
                                                                             {"preWrapMode", {{"type", "string"}}},
                                                                             {"postWrapMode", {{"type", "string"}}},
                                                                             {"pre", {{"type", "string"}}},
                                                                             {"post", {{"type", "string"}}}}},
                                         {"required", mcp::Json::array({"keys"})}}})}};
}

mcp::Json curvecodec::gradientSchema() {
    return {{"description", "A gradient: {colorKeys: [[time, r, g, b]] (sRGB 0-1), alphaKeys: [[time, alpha]], mode?: blend|fixed, "
                            "linear?: bool}; at most 8 keys of each. Also {mode, colors: [[time, \"#RRGGBB\" | [r, g, b] linear]], "
                            "alphas: [[time, alpha]]} and one colour (\"#RRGGBB(AA)\" or [r, g, b(, a)] linear) for a constant gradient"},
            {"anyOf", mcp::Json::array({{{"type", "object"},
                                         {"properties", {{"colorKeys", {{"type", "array"}}}, {"alphaKeys", {{"type", "array"}}},
                                                         {"colors", {{"type", "array"}}}, {"alphas", {{"type", "array"}}},
                                                         {"mode", {{"type", "string"}, {"enum", mcp::Json::array({"blend", "fixed"})}}},
                                                         {"linear", {{"type", "boolean"}}}}}},
                                        {{"type", "string"}},
                                        {{"type", "array"}, {"minItems", 3}, {"maxItems", 4}, {"items", {{"type", "number"}}}}})}};
}
