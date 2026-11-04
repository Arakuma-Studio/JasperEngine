// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveJson.h"

#include "util/Color.h"
#include "util/NearestName.h"

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

    // a tangent: a number, or a step (infinite): null, or upstream's "inf" / "-inf" (MCP) and
    // the YAML spellings
    bool readTangent(const Json& j, float& out){
        if (j.is_null()){
            out = INFINITY;
            return true;
        }
        if (j.is_string()){
            const std::string text = j.get<std::string>();
            if (text == "inf" || text == "+inf" || text == ".inf" || text == "Infinity" || text == "+Infinity"){
                out = INFINITY;
                return true;
            }
            if (text == "-inf" || text == "-.inf" || text == "-Infinity"){
                out = -INFINITY;
                return true;
            }
            return false;
        }
        if (!j.is_number())
            return false;
        out = j.get<float>();
        return !std::isnan(out);
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

    void warnUnknownCurveKey(std::vector<Issue>& issues, const std::string& at, const std::string& key, const char* what, const std::vector<std::string>& known){
        issues.push_back({false, at, std::string("unknown ") + what + " key" + NearestNameSuffix(key, known), true});
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
            if (k.is_object()){
                // upstream's (lines and trails) key map {time, value, inTangent?, outTangent?}
                bool ok = k.contains("time") && k.contains("value") && readFloat(k["time"], key.time) && readFloat(k["value"], key.value);
                ok = ok && (!k.contains("inTangent") || readTangent(k["inTangent"], key.inTangent));
                ok = ok && (!k.contains("outTangent") || readTangent(k["outTangent"], key.outTangent));
                if (!ok){
                    fail(issues, path + indexed(i), "a key map is {time, value, inTangent, outTangent} (numbers; a step tangent: null or \"inf\")");
                    return false;
                }
                curve.keys.push_back(key);
                continue;
            }
            bool ok = k.is_array() && (k.size() == 2 || k.size() == 4 || k.size() == 5) && readFloat(k[0], key.time) &&
                      readFloat(k[1], key.value);
            if (ok && k.size() >= 4){
                // null (or upstream's "inf" / "-inf"): an infinite (step) tangent
                ok = readTangent(k[2], key.inTangent) && readTangent(k[3], key.outTangent);
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

    // upstream's wrap names (lines and trails: clamp, loop, ping_pong, and the spellings its
    // reader took); anything else reads as clamp with a warning, as it did there
    void readUpstreamWrap(const Json& j, CurveWrapMode& mode, const std::string& path, std::vector<Issue>& issues){
        const std::string name = j.is_string() ? j.get<std::string>() : std::string();
        if (name == "clamp"){
            mode = CurveWrapMode::Clamp;
        }else if (name == "loop" || name == "repeat"){
            mode = CurveWrapMode::Loop;
        }else if (name == "ping_pong" || name == "pingpong" || name == "pingPong"){
            mode = CurveWrapMode::PingPong;
        }else{
            mode = CurveWrapMode::Clamp;
            warn(issues, path, "not a wrap mode name (clamp, loop, ping_pong): clamp");
        }
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
    // upstream's (lines and trails) number: a constant curve
    if (j.is_number()){
        float value = 0.0f;
        if (!readFloat(j, value)){
            fail(issues, "", "a constant curve is a finite number");
            return false;
        }
        curve = Curve::constant(value);
        return true;
    }
    if (j.is_array()){
        if (!readKeys(j, curve, "", issues))
            return false;
        if (isUpstreamCurve(j))
            curve.normalize();
        return true;
    }
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
        }else if (key == "pre"){
            readUpstreamWrap(*it, curve.preWrapMode, ".pre", issues);
        }else if (key == "post"){
            readUpstreamWrap(*it, curve.postWrapMode, ".post", issues);
        }else{
            warnUnknownCurveKey(issues, "." + key, key, "curve", {"keys", "preWrapMode", "postWrapMode", "pre", "post"});
        }
    }
    if (!haveKeys){
        fail(issues, ".keys", "a curve object needs its keys");
        return false;
    }
    if (ok && isUpstreamCurve(j))
        curve.normalize();
    return ok;
}

bool curvejson::isUpstreamCurve(const Json& j){
    if (j.is_number())
        return true;
    const Json* keys = &j;
    if (j.is_object()){
        if (j.contains("pre") || j.contains("post"))
            return true;
        if (!j.contains("keys"))
            return false;
        keys = &j["keys"];
    }
    if (!keys->is_array())
        return false;
    for (const Json& k : *keys){
        if (k.is_object())
            return true;
        // a tangent as text ("inf"; a key's mode token is its 5th element)
        if (k.is_array() && k.size() >= 4 && (k[2].is_string() || k[3].is_string()))
            return true;
    }
    return false;
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

namespace {

    // upstream's (lines and trails) colour: "#RRGGBB(AA)" (sRGB) or [r, g, b(, a)] (linear),
    // as sRGB-encoded rgb (Gradient's storage) and alpha
    bool upstreamColor(const Json& j, Vector3& srgb, float& alpha, bool& hasAlpha){
        hasAlpha = false;
        alpha = 1.0f;
        if (j.is_string()){
            std::string hex = j.get<std::string>();
            if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
            if (hex.size() != 6 && hex.size() != 8) return false;
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (size_t i = 0; i < hex.size() / 2; i++){
                char* end = nullptr;
                const std::string pair = hex.substr(i * 2, 2);
                const long byte = std::strtol(pair.c_str(), &end, 16);
                if (!end || *end != '\0') return false;
                v[i] = (float)byte / 255.0f;
            }
            srgb = Vector3(v[0], v[1], v[2]);
            alpha = v[3];
            hasAlpha = hex.size() == 8;
            return true;
        }
        if (j.is_array() && (j.size() == 3 || j.size() == 4)){
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (size_t i = 0; i < j.size(); i++){
                if (!readFloat(j[i], v[i])) return false;
            }
            // linear values (HDR above 1 kept: the encoding extends past 1)
            srgb = Color::linearTosRGB(Vector3(std::max(v[0], 0.0f), std::max(v[1], 0.0f), std::max(v[2], 0.0f)));
            alpha = v[3];
            hasAlpha = j.size() == 4;
            return true;
        }
        return false;
    }

    // upstream's gradient: {mode, colors: [[time, colour]], alphas: [[time, alpha]]}, or one
    // colour (a constant gradient); Unity's 8 keys of each at most (the first 7 and the last)
    bool upstreamGradientFromJson(const Json& j, Gradient& g, std::vector<Issue>& issues){
        g = Gradient();
        Vector3 single;
        float alpha = 1.0f;
        bool hasAlpha = false;
        if (upstreamColor(j, single, alpha, hasAlpha)){
            g = Gradient::constant(single, alpha);
            return true;
        }
        if (!j.is_object()){
            fail(issues, "", "a gradient is {colorKeys, alphaKeys}, {mode, colors, alphas} or one colour");
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it){
            const std::string& key = it.key();
            const std::string at = "." + key;
            if (key == "mode"){
                const std::string mode = it->is_string() ? it->get<std::string>() : std::string();
                g.mode = mode == "fixed" ? GradientMode::Fixed : GradientMode::Blend;
                if (mode != "fixed" && mode != "blend")
                    warn(issues, at, "mode is not blend or fixed: blend");
            }else if (key == "colors"){
                if (!it->is_array()) continue;
                for (size_t i = 0; i < it->size(); i++){
                    const Json& k = (*it)[i];
                    float time = 0.0f;
                    Vector3 color;
                    float a = 1.0f;
                    bool withAlpha = false;
                    if (!k.is_array() || k.size() < 2 || !readFloat(k[0], time) || !upstreamColor(k[1], color, a, withAlpha)){
                        warn(issues, at + indexed(i), "not a colour key [time, \"#RRGGBB\" | [r, g, b]]: skipped");
                        continue;
                    }
                    if (time < 0.0f || time > 1.0f)
                        warn(issues, at + indexed(i), "a key's time is 0..1: clamped");
                    g.colorKeys.push_back({time, color});
                }
            }else if (key == "alphas"){
                if (!it->is_array()) continue;
                for (size_t i = 0; i < it->size(); i++){
                    const Json& k = (*it)[i];
                    float v[2] = {0.0f, 1.0f};
                    if (!k.is_array() || k.size() < 2 || !readFloat(k[0], v[0]) || !readFloat(k[1], v[1])){
                        warn(issues, at + indexed(i), "not an alpha key [time, alpha]: skipped");
                        continue;
                    }
                    if (v[0] < 0.0f || v[0] > 1.0f)
                        warn(issues, at + indexed(i), "a key's time is 0..1: clamped");
                    g.alphaKeys.push_back({v[0], v[1]});
                }
            }else{
                warnUnknownCurveKey(issues, at, key, "gradient", {"mode", "colors", "alphas"});
            }
        }
        if (g.colorKeys.size() > Gradient::kMaxKeys || g.alphaKeys.size() > Gradient::kMaxKeys)
            warn(issues, "", "more than 8 colour or alpha keys: the first 7 and the last are kept");
        g.normalize();
        return true;
    }

}

bool curvejson::isUpstreamGradient(const Json& j){
    return j.is_string() || j.is_array() || (j.is_object() && (j.contains("colors") || j.contains("alphas")));
}

bool curvejson::gradientFromJson(const Json& j, Gradient& g, std::vector<Issue>& issues){
    if (isUpstreamGradient(j))
        return upstreamGradientFromJson(j, g, issues);
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
            warnUnknownCurveKey(issues, at, key, "gradient", {"colorKeys", "alphaKeys", "mode", "linear"});
        }
    }
    return ok;
}
