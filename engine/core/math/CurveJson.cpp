// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::curvejson;

namespace {

    struct ModeName {
        CurveTangentMode mode;
        const char* name;
    };

    constexpr ModeName kTangentModes[] = {
        {CurveTangentMode::Free, "free"},
        {CurveTangentMode::Auto, "auto"},
        {CurveTangentMode::Linear, "linear"},
        {CurveTangentMode::Constant, "constant"},
        {CurveTangentMode::ClampedAuto, "clampedAuto"},
    };

    struct WrapName {
        CurveWrapMode mode;
        const char* name;
    };

    constexpr WrapName kWrapModes[] = {
        {CurveWrapMode::Clamp, "clamp"},
        {CurveWrapMode::Loop, "loop"},
        {CurveWrapMode::PingPong, "pingPong"},
        {CurveWrapMode::Linear, "linear"},
        {CurveWrapMode::LoopWithOffset, "loopWithOffset"},
    };

    Json jf(float value){
        return Json(jsonFloat(value));
    }

    Json jtangent(float value){
        return std::isfinite(value) ? jf(value) : Json(nullptr);
    }

    bool readFloat(const Json& j, float& out){
        if (!j.is_number())
            return false;
        out = j.get<float>();
        return std::isfinite(out);
    }

    template<size_t N>
    bool readFloats(const Json& j, float (&out)[N]){
        if (!j.is_array() || j.size() != N)
            return false;
        for (size_t i = 0; i < N; i++){
            if (!readFloat(j[i], out[i]))
                return false;
        }
        return true;
    }

    std::string indexed(size_t index){
        return "[" + std::to_string(index) + "]";
    }

    void fail(std::vector<Issue>& issues, const std::string& path, const std::string& message){
        issues.push_back({true, path, message});
    }

    void warn(std::vector<Issue>& issues, const std::string& path, const std::string& message){
        issues.push_back({false, path, message});
    }

    bool readKeys(const Json& j, Curve& curve, const std::string& path, std::vector<Issue>& issues){
        if (!j.is_array()){
            fail(issues, path, "a curve's keys are an array of [time, value, inTangent, outTangent(, mode)]");
            return false;
        }
        curve.keys.clear();
        for (size_t i = 0; i < j.size(); i++){
            const Json& k = j[i];
            CurveKey key;
            bool ok = k.is_array() && (k.size() == 2 || k.size() == 4 || k.size() == 5) && readFloat(k[0], key.time) &&
                      readFloat(k[1], key.value);
            if (ok && k.size() >= 4){
                // null: an infinite (step) tangent
                key.inTangent = k[2].is_null() ? INFINITY : 0.0f;
                key.outTangent = k[3].is_null() ? INFINITY : 0.0f;
                ok = (k[2].is_null() || readFloat(k[2], key.inTangent)) && (k[3].is_null() || readFloat(k[3], key.outTangent));
            }
            if (ok && k.size() == 5){
                if (!k[4].is_string() || !modeFromToken(k[4].get<std::string>(), key.mode)){
                    fail(issues, path + indexed(i) + "[4]",
                         "a key's mode is clampedAuto, auto, free, linear, constant or in:<mode>,out:<mode>[,broken]");
                    return false;
                }
            }
            if (!ok){
                fail(issues, path + indexed(i), "a key is [time, value], [time, value, inTangent, outTangent] or with a mode token (null tangent: a step)");
                return false;
            }
            curve.keys.push_back(key);
        }
        return true;
    }

    bool readWrap(const Json& j, CurveWrapMode& mode, const std::string& path, std::vector<Issue>& issues){
        if (!j.is_string() || !wrapModeFromName(j.get<std::string>(), mode)){
            fail(issues, path, "a wrap mode is clamp, loop, pingPong, linear or loopWithOffset");
            return false;
        }
        return true;
    }

}

double curvejson::jsonFloat(float value){
    if (!std::isfinite(value))
        return 0.0;
    char text[32];
    for (int precision = 1; precision <= 9; precision++){
        std::snprintf(text, sizeof(text), "%.*g", precision, (double)value);
        const double parsed = std::strtod(text, nullptr);
        if ((float)parsed == value)
            return parsed;
    }
    return (double)value;
}

const char* curvejson::tangentModeName(CurveTangentMode mode){
    for (const ModeName& m : kTangentModes){
        if (m.mode == mode) return m.name;
    }
    return "free";
}

bool curvejson::tangentModeFromName(const std::string& name, CurveTangentMode& mode){
    for (const ModeName& m : kTangentModes){
        if (name == m.name){
            mode = m.mode;
            return true;
        }
    }
    return false;
}

const char* curvejson::wrapModeName(CurveWrapMode mode){
    for (const WrapName& w : kWrapModes){
        if (w.mode == mode) return w.name;
    }
    return "clamp";
}

bool curvejson::wrapModeFromName(const std::string& name, CurveWrapMode& mode){
    for (const WrapName& w : kWrapModes){
        if (name == w.name){
            mode = w.mode;
            return true;
        }
    }
    return false;
}

std::string curvejson::modeToken(uint8_t mode){
    if (mode == 0)
        return std::string();
    CurveKey key;
    key.mode = mode;
    if (!key.broken() && key.inMode() == key.outMode())
        return tangentModeName(key.inMode());
    std::string token = std::string("in:") + tangentModeName(key.inMode()) + ",out:" + tangentModeName(key.outMode());
    if (key.broken())
        token += ",broken";
    return token;
}

bool curvejson::modeFromToken(const std::string& token, uint8_t& mode){
    CurveKey key;
    CurveTangentMode single;
    if (tangentModeFromName(token, single)){
        key.setBothModes(single);
        mode = key.mode;
        return true;
    }
    // in:<mode>,out:<mode>[,broken], the parts in any order
    bool haveIn = false, haveOut = false;
    size_t start = 0;
    while (start <= token.size()){
        const size_t end = token.find(',', start);
        const std::string part = token.substr(start, end == std::string::npos ? std::string::npos : end - start);
        CurveTangentMode m;
        if (part == "broken"){
            key.setBroken(true);
        }else if (part.rfind("in:", 0) == 0 && !haveIn && tangentModeFromName(part.substr(3), m)){
            key.setInMode(m);
            haveIn = true;
        }else if (part.rfind("out:", 0) == 0 && !haveOut && tangentModeFromName(part.substr(4), m)){
            key.setOutMode(m);
            haveOut = true;
        }else{
            return false;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (!haveIn && !haveOut)
        return false;
    mode = key.mode;
    return true;
}

Json curvejson::curveToJson(const Curve& curve){
    Json keys = Json::array();
    for (const CurveKey& k : curve.keys){
        Json key = Json::array({jf(k.time), jf(k.value), jtangent(k.inTangent), jtangent(k.outTangent)});
        if (k.mode != 0)
            key.push_back(modeToken(k.mode));
        keys.push_back(std::move(key));
    }
    if (curve.preWrapMode == CurveWrapMode::Clamp && curve.postWrapMode == CurveWrapMode::Clamp)
        return keys;
    Json j = Json::object();
    j["keys"] = std::move(keys);
    if (curve.preWrapMode != CurveWrapMode::Clamp)
        j["preWrapMode"] = wrapModeName(curve.preWrapMode);
    if (curve.postWrapMode != CurveWrapMode::Clamp)
        j["postWrapMode"] = wrapModeName(curve.postWrapMode);
    return j;
}

bool curvejson::curveFromJson(const Json& j, Curve& curve, std::vector<Issue>& issues){
    curve = Curve();
    if (j.is_array())
        return readKeys(j, curve, "", issues);
    if (!j.is_object()){
        fail(issues, "", "a curve is an array of [time, value, inTangent, outTangent] keys, or {keys, preWrapMode, postWrapMode}");
        return false;
    }
    bool ok = true;
    bool haveKeys = false;
    for (auto it = j.begin(); it != j.end(); ++it){
        const std::string& key = it.key();
        if (key == "keys"){
            haveKeys = true;
            ok = readKeys(*it, curve, ".keys", issues) && ok;
        }else if (key == "preWrapMode"){
            ok = readWrap(*it, curve.preWrapMode, ".preWrapMode", issues) && ok;
        }else if (key == "postWrapMode"){
            ok = readWrap(*it, curve.postWrapMode, ".postWrapMode", issues) && ok;
        }else{
            warn(issues, "." + key, "unknown curve key");
        }
    }
    if (!haveKeys){
        fail(issues, ".keys", "a curve object needs its keys");
        return false;
    }
    return ok;
}

Json curvejson::gradientToJson(const Gradient& g){
    Json j = Json::object();
    Json colors = Json::array();
    for (const GradientColorKey& k : g.colorKeys){
        colors.push_back(Json::array({jf(k.time), jf(k.color.x), jf(k.color.y), jf(k.color.z)}));
    }
    Json alphas = Json::array();
    for (const GradientAlphaKey& k : g.alphaKeys){
        alphas.push_back(Json::array({jf(k.time), jf(k.alpha)}));
    }
    j["colorKeys"] = colors;
    j["alphaKeys"] = alphas;
    if (g.mode != GradientMode::Blend)
        j["mode"] = "fixed";
    if (g.linear)
        j["linear"] = true;
    return j;
}

bool curvejson::gradientFromJson(const Json& j, Gradient& g, std::vector<Issue>& issues){
    if (!j.is_object()){
        fail(issues, "", "a gradient is {colorKeys: [[time, r, g, b]], alphaKeys: [[time, a]]}");
        return false;
    }
    g = Gradient();
    bool ok = true;
    for (auto it = j.begin(); it != j.end(); ++it){
        const std::string& key = it.key();
        const std::string at = "." + key;
        if (key == "colorKeys"){
            if (!it->is_array()){ fail(issues, at, "colorKeys is an array of [time, r, g, b]"); ok = false; continue; }
            for (size_t i = 0; i < it->size(); i++){
                float v[4];
                if (!readFloats((*it)[i], v)){ fail(issues, at + indexed(i), "a colour key is [time, r, g, b] (sRGB)"); ok = false; break; }
                g.colorKeys.push_back({v[0], Vector3(v[1], v[2], v[3])});
            }
        }else if (key == "alphaKeys"){
            if (!it->is_array()){ fail(issues, at, "alphaKeys is an array of [time, alpha]"); ok = false; continue; }
            for (size_t i = 0; i < it->size(); i++){
                float v[2];
                if (!readFloats((*it)[i], v)){ fail(issues, at + indexed(i), "an alpha key is [time, alpha]"); ok = false; break; }
                g.alphaKeys.push_back({v[0], v[1]});
            }
        }else if (key == "mode"){
            const std::string mode = it->is_string() ? it->get<std::string>() : std::string();
            if (mode == "blend"){
                g.mode = GradientMode::Blend;
            }else if (mode == "fixed"){
                g.mode = GradientMode::Fixed;
            }else{
                fail(issues, at, "mode is one of blend, fixed");
                ok = false;
            }
        }else if (key == "linear"){
            if (!it->is_boolean()){ fail(issues, at, "linear is true or false"); ok = false; continue; }
            g.linear = it->get<bool>();
        }else{
            warn(issues, at, "unknown gradient key");
        }
    }
    return ok;
}
