// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphParamValues.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using graph::Json;

namespace{

    bool number(const Json& object, const char* key, float& out){
        auto it = object.find(key);
        if (it == object.end() || !it->is_number()) return false;
        out = it->get<float>();
        return std::isfinite(out);
    }

    // a float stored as JSON without float noise ("0.3" stays 0.3, not 0.30000001192...)
    Json rounded(float value){
        return std::round((double)value * 1e6) / 1e6;
    }

}

bool graphvalues::colorFrom(const Json& value, Vector4& out){
    if (value.is_object()){
        float r, g, b, a = 1.0f;
        if (!number(value, "r", r) || !number(value, "g", g) || !number(value, "b", b)) return false;
        if (value.contains("a") && !number(value, "a", a)) return false;
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

Json graphvalues::colorJson(const Vector4& color, const Json& like){
    if (like.is_array()){
        Json out = Json::array({rounded(color.x), rounded(color.y), rounded(color.z)});
        if (like.size() != 3) out.push_back(rounded(color.w));
        return out;
    }
    Json out = like.is_object() ? like : Json::object();
    out["r"] = rounded(color.x);
    out["g"] = rounded(color.y);
    out["b"] = rounded(color.z);
    out["a"] = rounded(color.w);
    return out;
}

bool graphvalues::gradientFrom(const Json& value, Gradient& out){
    if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return false;
    Gradient gradient;
    for (const Json& point : value["points"]){
        if (!point.is_object()) return false;
        float pos, r, g, b, a = 1.0f;
        if (!number(point, "pos", pos) || !number(point, "r", r) || !number(point, "g", g) || !number(point, "b", b)) return false;
        if (point.contains("a") && !number(point, "a", a)) return false;
        gradient.colorKeys.push_back({pos, Vector3(r, g, b)});
        gradient.alphaKeys.push_back({pos, a});
    }
    gradient.mode = value.value("interpolation", 1) == 0 ? GradientMode::Fixed : GradientMode::Blend;
    gradient.sortKeys();
    out = gradient;
    return true;
}

bool graphvalues::gradientJson(const Gradient& gradient, const Json& like, Json& out, std::string& error){
    std::vector<float> times;
    for (const GradientColorKey& key : gradient.colorKeys) times.push_back(key.time);
    for (const GradientAlphaKey& key : gradient.alphaKeys) times.push_back(key.time);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() > RAMP_KEYS_MAX){
        error = "a gradient holds at most " + std::to_string(RAMP_KEYS_MAX) + " keys (colour and alpha keys at different places count apart)";
        return false;
    }
    out = like.is_object() ? like : Json::object();
    Json points = Json::array();
    for (float t : times){
        const Vector3 color = gradient.evaluateEncoded(t);
        points.push_back({{"pos", rounded(t)}, {"r", rounded(color.x)}, {"g", rounded(color.y)}, {"b", rounded(color.z)},
                          {"a", rounded(gradient.evaluateAlpha(t))}});
    }
    out["points"] = std::move(points);
    // Blend keeps a smooth interpolation the file had (2, 3): the editor shows those as Blend
    const int before = like.is_object() ? like.value("interpolation", 1) : 1;
    if (gradient.mode == GradientMode::Fixed) out["interpolation"] = 0;
    else out["interpolation"] = before == 0 ? 1 : before;
    return true;
}

bool graphvalues::curveFrom(const Json& value, Curve& out){
    if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return false;
    Curve curve;
    for (const Json& point : value["points"]){
        if (!point.is_object()) return false;
        CurveKey key;
        float ls = 0.0f, rs = 0.0f;
        if (!number(point, "x", key.time) || !number(point, "y", key.value)) return false;
        number(point, "ls", ls);
        number(point, "rs", rs);
        key.inTangent = ls;
        key.outTangent = rs;
        curve.keys.push_back(key);
    }
    std::stable_sort(curve.keys.begin(), curve.keys.end(), [](const CurveKey& a, const CurveKey& b) { return a.time < b.time; });
    out = curve;
    return true;
}

bool graphvalues::curveJson(const Curve& curve, const Json& like, Json& out, std::string& error){
    if (curve.keys.size() > RAMP_KEYS_MAX){
        error = "a curve holds at most " + std::to_string(RAMP_KEYS_MAX) + " keys";
        return false;
    }
    out = like.is_object() ? like : Json::object();
    Json points = Json::array();
    for (const CurveKey& key : curve.keys){
        // an infinite tangent (a step) has no slope a file can hold: flat
        const float ls = std::isfinite(key.inTangent) ? key.inTangent : 0.0f;
        const float rs = std::isfinite(key.outTangent) ? key.outTangent : 0.0f;
        points.push_back({{"x", rounded(key.time)}, {"y", rounded(key.value)}, {"ls", rounded(ls)}, {"rs", rounded(rs)}});
    }
    out["points"] = std::move(points);
    return true;
}
