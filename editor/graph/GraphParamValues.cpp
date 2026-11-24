// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphParamValues.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using GraphJson = graph::Json;

namespace{

    bool paramNumber(const GraphJson& object, const char* key, float& out){
        auto it = object.find(key);
        if (it == object.end() || !it->is_number()) return false;
        out = it->get<float>();
        return std::isfinite(out);
    }

    // a whole number at `key`, `fallback` when it is missing or of another type (a file holds anything)
    int paramWholeOr(const GraphJson& object, const char* key, int fallback){
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? (int)it->get<double>() : fallback;
    }

    // a float stored as JSON without float noise ("0.3" stays 0.3, not 0.30000001192...)
    GraphJson paramRounded(float value){
        return std::round((double)value * 1e6) / 1e6;
    }

}

void graphvalues::dragRange(const std::optional<double>& min, const std::optional<double>& max, bool whole, double& lo, double& hi){
    const double far = whole ? (double)std::numeric_limits<int>::max() : (double)std::numeric_limits<float>::max();
    lo = min.value_or(-far);
    hi = max.value_or(far);
}

bool graphvalues::colorFrom(const GraphJson& value, Vector4& out){
    if (value.is_object()){
        float r, g, b, a = 1.0f;
        if (!paramNumber(value, "r", r) || !paramNumber(value, "g", g) || !paramNumber(value, "b", b)) return false;
        if (value.contains("a") && !paramNumber(value, "a", a)) return false;
        out = Vector4(r, g, b, a);
        return true;
    }
    if (value.is_array() && (value.size() == 3 || value.size() == 4)){
        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (size_t i = 0; i < value.size(); i++){
            if (!value[i].is_number()) return false;
            c[i] = value[i].get<float>();
        }
        out = Vector4(c[0], c[1], c[2], c[3]);
        return true;
    }
    return false;
}

GraphJson graphvalues::colorJson(const Vector4& color, const GraphJson& like){
    if (like.is_array()){
        GraphJson out = GraphJson::array({paramRounded(color.x), paramRounded(color.y), paramRounded(color.z)});
        if (like.size() != 3) out.push_back(paramRounded(color.w));
        return out;
    }
    GraphJson out = like.is_object() ? like : GraphJson::object();
    out["r"] = paramRounded(color.x);
    out["g"] = paramRounded(color.y);
    out["b"] = paramRounded(color.z);
    out["a"] = paramRounded(color.w);
    return out;
}

bool graphvalues::gradientFrom(const GraphJson& value, Gradient& out){
    if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return false;
    Gradient gradient;
    for (const GraphJson& point : value["points"]){
        if (!point.is_object()) return false;
        float pos, r, g, b, a = 1.0f;
        if (!paramNumber(point, "pos", pos) || !paramNumber(point, "r", r) || !paramNumber(point, "g", g) || !paramNumber(point, "b", b)) return false;
        if (point.contains("a") && !paramNumber(point, "a", a)) return false;
        gradient.colorKeys.push_back({pos, Vector3(r, g, b)});
        gradient.alphaKeys.push_back({pos, a});
    }
    gradient.mode = paramWholeOr(value, "interpolation", 1) == 0 ? GradientMode::Fixed : GradientMode::Blend;
    gradient.sortKeys();
    out = gradient;
    return true;
}

bool graphvalues::gradientJson(const Gradient& gradient, const GraphJson& like, GraphJson& out, std::string& error){
    std::vector<float> times;
    for (const GradientColorKey& key : gradient.colorKeys) times.push_back(key.time);
    for (const GradientAlphaKey& key : gradient.alphaKeys) times.push_back(key.time);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    // (a file's ramp over the limit still edits down)
    const size_t had = like.is_object() && like.contains("points") && like["points"].is_array() ? like["points"].size() : 0;
    if (times.size() > std::max(RAMP_KEYS_MAX, had)){
        error = "a gradient holds at most " + std::to_string(RAMP_KEYS_MAX) + " keys (colour and alpha keys at different places count apart)";
        return false;
    }
    out = like.is_object() ? like : GraphJson::object();
    GraphJson points = GraphJson::array();
    for (float t : times){
        const Vector3 color = gradient.evaluateEncoded(t);
        points.push_back({{"pos", paramRounded(t)}, {"r", paramRounded(color.x)}, {"g", paramRounded(color.y)}, {"b", paramRounded(color.z)},
                          {"a", paramRounded(gradient.evaluateAlpha(t))}});
    }
    out["points"] = std::move(points);
    // Blend keeps a smooth interpolation the file had (2, 3): the editor shows those as Blend
    const int before = paramWholeOr(like, "interpolation", 1);
    if (gradient.mode == GradientMode::Fixed) out["interpolation"] = 0;
    else out["interpolation"] = before == 0 ? 1 : before;
    return true;
}

bool graphvalues::curveFrom(const GraphJson& value, Curve& out){
    if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return false;
    Curve curve;
    for (const GraphJson& point : value["points"]){
        if (!point.is_object()) return false;
        CurveKey key;
        float ls = 0.0f, rs = 0.0f;
        if (!paramNumber(point, "x", key.time) || !paramNumber(point, "y", key.value)) return false;
        paramNumber(point, "ls", ls);
        paramNumber(point, "rs", rs);
        key.inTangent = ls;
        key.outTangent = rs;
        curve.keys.push_back(key);
    }
    std::stable_sort(curve.keys.begin(), curve.keys.end(), [](const CurveKey& a, const CurveKey& b) { return a.time < b.time; });
    out = curve;
    return true;
}

bool graphvalues::curveJson(const Curve& curve, const GraphJson& like, GraphJson& out, std::string& error){
    const size_t had = like.is_object() && like.contains("points") && like["points"].is_array() ? like["points"].size() : 0;
    if (curve.keys.size() > std::max(RAMP_KEYS_MAX, had)){
        error = "a curve holds at most " + std::to_string(RAMP_KEYS_MAX) + " keys";
        return false;
    }
    out = like.is_object() ? like : GraphJson::object();
    GraphJson points = GraphJson::array();
    for (const CurveKey& key : curve.keys){
        // an infinite tangent (a step) has no slope a file can hold: flat
        const float ls = std::isfinite(key.inTangent) ? key.inTangent : 0.0f;
        const float rs = std::isfinite(key.outTangent) ? key.outTangent : 0.0f;
        points.push_back({{"x", paramRounded(key.time)}, {"y", paramRounded(key.value)}, {"ls", paramRounded(ls)}, {"rs", paramRounded(rs)}});
    }
    out["points"] = std::move(points);
    return true;
}
