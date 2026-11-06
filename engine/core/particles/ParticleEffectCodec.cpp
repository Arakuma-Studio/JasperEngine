// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Validation with path diagnostics follows OpenEngine's ParticleStackValidation
// (Engine/Modules/Particles/Source/ParticleStackValidation.cpp), Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "ParticleEffectCodec.h"

#include "math/CurveJson.h"
#include "particles/ParticleModules.h"
#include "util/NearestName.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace jasperengine;
using namespace jasperengine::particles;

using Json = nlohmann::ordered_json;

namespace {

    // ---------------------------------------------------------------- helpers

    void error(std::vector<Diagnostic>& out, const std::string& path, const std::string& message){
        out.push_back({Diagnostic::Severity::Error, path.empty() ? "$" : path, message});
    }

    void warn(std::vector<Diagnostic>& out, const std::string& path, const std::string& message){
        out.push_back({Diagnostic::Severity::Warning, path.empty() ? "$" : path, message});
    }

    // A key the format does not have: a warning here (old files must load), and applyMergePatch
    // refuses it. `known` holds what it may have meant.
    void warnUnknownKey(std::vector<Diagnostic>& out, const std::string& path, const std::string& key, const std::string& message, const std::vector<std::string>& known){
        out.push_back({Diagnostic::Severity::Warning, path, message + NearestNameSuffix(key, known), true});
    }

    std::string join(const std::string& path, const std::string& key){
        return path.empty() ? key : path + "." + key;
    }

    std::string indexed(const std::string& path, size_t index){
        return path + "[" + std::to_string(index) + "]";
    }

    Json jf(float value){
        return Json(ParticleEffectCodec::jsonFloat(value));
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

    const char* optionName(const std::vector<EnumOption>& options, uint8_t value){
        for (const EnumOption& o : options){
            if (o.value == value) return o.json;
        }
        return options.front().json;
    }

    bool optionValue(const std::vector<EnumOption>& options, const Json& j, uint8_t& value){
        if (!j.is_string())
            return false;
        const std::string name = j.get<std::string>();
        for (const EnumOption& o : options){
            if (name == o.json){
                value = o.value;
                return true;
            }
        }
        return false;
    }

    std::string optionList(const std::vector<EnumOption>& options){
        std::string list;
        for (const EnumOption& o : options){
            if (!list.empty()) list += ", ";
            list += o.json;
        }
        return list;
    }

    std::string optionList(const FieldDesc& field){
        std::string list;
        for (uint8_t i = 0; i < field.optionCount; i++){
            if (!list.empty()) list += ", ";
            list += field.options[i].json;
        }
        return list;
    }

    // ---------------------------------------------------------------- curves and gradients

    // the shared encoding (math/CurveJson.h), its issues as this file's diagnostics

    bool reportIssues(std::vector<Diagnostic>& out, const std::string& path, const std::vector<curvejson::Issue>& issues){
        bool ok = true;
        for (const curvejson::Issue& issue : issues){
            if (issue.error){
                error(out, path + issue.path, issue.message);
                ok = false;
            }else{
                warn(out, path + issue.path, issue.message);
                out.back().unknownKey = issue.unknownKey;
            }
        }
        return ok;
    }

    Json curveToJson(const Curve& curve){
        return curvejson::curveToJson(curve);
    }

    bool curveFromJson(const Json& j, Curve& curve, const std::string& path, std::vector<Diagnostic>& out){
        std::vector<curvejson::Issue> issues;
        const bool read = curvejson::curveFromJson(j, curve, issues);
        return reportIssues(out, path, issues) && read;
    }

    Json gradientToJson(const Gradient& g){
        return curvejson::gradientToJson(g);
    }

    bool gradientFromJson(const Json& j, Gradient& g, const std::string& path, std::vector<Diagnostic>& out){
        std::vector<curvejson::Issue> issues;
        const bool read = curvejson::gradientFromJson(j, g, issues);
        return reportIssues(out, path, issues) && read;
    }

    // ---------------------------------------------------------------- MinMaxCurve

    bool isPlainConstant(const MinMaxCurve& v){
        const MinMaxCurve d;
        return v.mode == CurveMode::Constant && v.constantMax == v.constant && v.curve.empty() && v.curveMax.empty() &&
               v.multiplier == d.multiplier && v.driver == d.driver && v.driverMin == d.driverMin && v.driverMax == d.driverMax && v.wrap == d.wrap;
    }

    Json curveValueToJson(const MinMaxCurve& v){
        if (isPlainConstant(v))
            return jf(v.constant);
        const MinMaxCurve d;
        Json j = Json::object();
        j["mode"] = optionName(ParticleModules::curveModes(), (uint8_t)v.mode);
        if (v.constant != d.constant) j["constant"] = jf(v.constant);
        if (v.constantMax != d.constantMax) j["constantMax"] = jf(v.constantMax);
        if (!v.curve.empty()) j["curve"] = curveToJson(v.curve);
        if (!v.curveMax.empty()) j["curveMax"] = curveToJson(v.curveMax);
        if (v.multiplier != d.multiplier) j["multiplier"] = jf(v.multiplier);
        if (v.driver != d.driver) j["driver"] = optionName(ParticleModules::curveDrivers(), (uint8_t)v.driver);
        if (v.driverMin != d.driverMin) j["driverMin"] = jf(v.driverMin);
        if (v.driverMax != d.driverMax) j["driverMax"] = jf(v.driverMax);
        if (v.wrap != d.wrap) j["wrap"] = optionName(ParticleModules::curveWraps(), (uint8_t)v.wrap);
        return j;
    }

    bool curveValueFromJson(const Json& j, MinMaxCurve& v, const std::string& path, std::vector<Diagnostic>& out){
        if (j.is_number()){
            float c = 0.0f;
            if (!readFloat(j, c)){ error(out, path, "not a finite number"); return false; }
            v = MinMaxCurve(c);
            return true;
        }
        if (!j.is_object()){
            error(out, path, "a value is a number or {mode, constant, constantMax, curve, curveMax, multiplier, driver, driverMin, driverMax, wrap}");
            return false;
        }
        v = MinMaxCurve();
        bool ok = true;
        bool hasMode = false;
        for (auto it = j.begin(); it != j.end(); ++it){
            const std::string& key = it.key();
            const std::string at = join(path, key);
            if (key == "mode"){
                uint8_t mode = 0;
                hasMode = true;
                if (!optionValue(ParticleModules::curveModes(), *it, mode)){ error(out, at, "mode is one of " + optionList(ParticleModules::curveModes())); ok = false; }
                v.mode = (CurveMode)mode;
            }else if (key == "constant" || key == "constantMax" || key == "multiplier" || key == "driverMin" || key == "driverMax"){
                float f = 0.0f;
                if (!readFloat(*it, f)){ error(out, at, "not a finite number"); ok = false; continue; }
                if (key == "constant") v.constant = f;
                else if (key == "constantMax") v.constantMax = f;
                else if (key == "multiplier") v.multiplier = f;
                else if (key == "driverMin") v.driverMin = f;
                else v.driverMax = f;
            }else if (key == "curve"){
                ok = curveFromJson(*it, v.curve, at, out) && ok;
            }else if (key == "curveMax"){
                ok = curveFromJson(*it, v.curveMax, at, out) && ok;
            }else if (key == "driver"){
                uint8_t driver = 0;
                if (!optionValue(ParticleModules::curveDrivers(), *it, driver)){ error(out, at, "driver is one of " + optionList(ParticleModules::curveDrivers())); ok = false; }
                v.driver = (CurveDriver)driver;
            }else if (key == "wrap"){
                uint8_t wrap = 0;
                if (!optionValue(ParticleModules::curveWraps(), *it, wrap)){ error(out, at, "wrap is one of " + optionList(ParticleModules::curveWraps())); ok = false; }
                v.wrap = (CurveWrap)wrap;
            }else{
                warnUnknownKey(out, at, key, "unknown value key",
                               {"mode", "constant", "constantMax", "curve", "curveMax", "multiplier", "driver", "driverMin", "driverMax", "wrap"});
            }
        }
        if (!hasMode){
            // {constant: x} alone, or {constant, constantMax} for a range
            v.mode = j.contains("curveMax") ? CurveMode::TwoCurves : j.contains("curve") ? CurveMode::Curve :
                     j.contains("constantMax") ? CurveMode::TwoConstants : CurveMode::Constant;
            if (v.mode == CurveMode::Constant) v.constantMax = v.constant;
        }
        return ok;
    }

    // ---------------------------------------------------------------- MinMaxGradient

    Json colorToJson(const Vector4& c){
        return Json::array({jf(c.x), jf(c.y), jf(c.z), jf(c.w)});
    }

    bool colorFromJson(const Json& j, Vector4& c){
        if (j.is_array() && j.size() == 3){
            float v[3];
            if (!readFloats(j, v)) return false;
            c = Vector4(v[0], v[1], v[2], 1.0f);
            return true;
        }
        float v[4];
        if (!readFloats(j, v)) return false;
        c = Vector4(v[0], v[1], v[2], v[3]);
        return true;
    }

    Json gradientValueToJson(const MinMaxGradient& v){
        const MinMaxGradient d;
        if (v.mode == GradientValueMode::Color && v.colorMax == v.color && v.gradient == d.gradient && v.gradientMax == d.gradientMax &&
                v.driver == d.driver && v.driverMin == d.driverMin && v.driverMax == d.driverMax && v.wrap == d.wrap)
            return colorToJson(v.color);
        Json j = Json::object();
        j["mode"] = optionName(ParticleModules::gradientModes(), (uint8_t)v.mode);
        if (v.color != d.color) j["color"] = colorToJson(v.color);
        if (v.colorMax != d.colorMax) j["colorMax"] = colorToJson(v.colorMax);
        if (v.gradient != d.gradient) j["gradient"] = gradientToJson(v.gradient);
        if (v.gradientMax != d.gradientMax) j["gradientMax"] = gradientToJson(v.gradientMax);
        if (v.driver != d.driver) j["driver"] = optionName(ParticleModules::curveDrivers(), (uint8_t)v.driver);
        if (v.driverMin != d.driverMin) j["driverMin"] = jf(v.driverMin);
        if (v.driverMax != d.driverMax) j["driverMax"] = jf(v.driverMax);
        if (v.wrap != d.wrap) j["wrap"] = optionName(ParticleModules::curveWraps(), (uint8_t)v.wrap);
        return j;
    }

    bool gradientValueFromJson(const Json& j, MinMaxGradient& v, const std::string& path, std::vector<Diagnostic>& out){
        if (j.is_array()){
            Vector4 c;
            if (!colorFromJson(j, c)){ error(out, path, "a colour is [r, g, b] or [r, g, b, a] (sRGB)"); return false; }
            v = MinMaxGradient(c);
            return true;
        }
        if (!j.is_object()){
            error(out, path, "a colour value is [r, g, b, a] or {mode, color, colorMax, gradient, gradientMax}");
            return false;
        }
        v = MinMaxGradient();
        bool ok = true;
        bool hasMode = false;
        for (auto it = j.begin(); it != j.end(); ++it){
            const std::string& key = it.key();
            const std::string at = join(path, key);
            if (key == "mode"){
                uint8_t mode = 0;
                hasMode = true;
                if (!optionValue(ParticleModules::gradientModes(), *it, mode)){ error(out, at, "mode is one of " + optionList(ParticleModules::gradientModes())); ok = false; }
                v.mode = (GradientValueMode)mode;
            }else if (key == "color" || key == "colorMax"){
                Vector4 c;
                if (!colorFromJson(*it, c)){ error(out, at, "a colour is [r, g, b] or [r, g, b, a] (sRGB)"); ok = false; continue; }
                (key == "color" ? v.color : v.colorMax) = c;
            }else if (key == "gradient"){
                ok = gradientFromJson(*it, v.gradient, at, out) && ok;
            }else if (key == "gradientMax"){
                ok = gradientFromJson(*it, v.gradientMax, at, out) && ok;
            }else if (key == "driver"){
                uint8_t driver = 0;
                if (!optionValue(ParticleModules::curveDrivers(), *it, driver)){ error(out, at, "driver is one of " + optionList(ParticleModules::curveDrivers())); ok = false; }
                v.driver = (CurveDriver)driver;
            }else if (key == "driverMin" || key == "driverMax"){
                float f = 0.0f;
                if (!readFloat(*it, f)){ error(out, at, "not a finite number"); ok = false; continue; }
                (key == "driverMin" ? v.driverMin : v.driverMax) = f;
            }else if (key == "wrap"){
                uint8_t wrap = 0;
                if (!optionValue(ParticleModules::curveWraps(), *it, wrap)){ error(out, at, "wrap is one of " + optionList(ParticleModules::curveWraps())); ok = false; }
                v.wrap = (CurveWrap)wrap;
            }else{
                warnUnknownKey(out, at, key, "unknown colour value key",
                               {"mode", "color", "colorMax", "gradient", "gradientMax", "driver", "driverMin", "driverMax", "wrap"});
            }
        }
        if (!hasMode){
            v.mode = j.contains("gradientMax") ? GradientValueMode::TwoGradients : j.contains("gradient") ? GradientValueMode::Gradient :
                     j.contains("colorMax") ? GradientValueMode::TwoColors : GradientValueMode::Color;
            if (v.mode == GradientValueMode::Color) v.colorMax = v.color;
        }
        return ok;
    }

    // ---------------------------------------------------------------- generic fields

    bool fieldEquals(const FieldDesc& f, const void* a, const void* b);

    bool structEquals(const StructDesc& desc, const void* a, const void* b){
        for (const FieldDesc& f : desc.fields){
            if (!fieldEquals(f, f.field(a), f.field(b)))
                return false;
        }
        return true;
    }

    bool fieldEquals(const FieldDesc& f, const void* a, const void* b){
        switch (f.type){
            case FieldType::Bool: return *(const bool*)a == *(const bool*)b;
            case FieldType::Int: return *(const int*)a == *(const int*)b;
            case FieldType::UInt:
            case FieldType::Mask: return *(const uint32_t*)a == *(const uint32_t*)b;
            case FieldType::Float: return *(const float*)a == *(const float*)b;
            case FieldType::Enum: return *(const uint8_t*)a == *(const uint8_t*)b;
            case FieldType::Vector2: return *(const Vector2*)a == *(const Vector2*)b;
            case FieldType::Vector3: return *(const Vector3*)a == *(const Vector3*)b;
            case FieldType::Vector4:
            case FieldType::Color: return *(const Vector4*)a == *(const Vector4*)b;
            case FieldType::String: return *(const std::string*)a == *(const std::string*)b;
            case FieldType::MinMaxCurve: return *(const MinMaxCurve*)a == *(const MinMaxCurve*)b;
            case FieldType::MinMaxGradient: return *(const MinMaxGradient*)a == *(const MinMaxGradient*)b;
            case FieldType::Curve: return *(const Curve*)a == *(const Curve*)b;
            case FieldType::Gradient: return *(const Gradient*)a == *(const Gradient*)b;
            case FieldType::Struct: return structEquals(*f.element, a, b);
            case FieldType::IntArray: return *(const std::vector<int>*)a == *(const std::vector<int>*)b;
            case FieldType::StringArray: return *(const std::vector<std::string>*)a == *(const std::vector<std::string>*)b;
            case FieldType::Array: {
                const size_t n = f.array.size(a);
                if (n != f.array.size(b)) return false;
                for (size_t i = 0; i < n; i++){
                    if (!structEquals(*f.element, f.array.at(const_cast<void*>(a), i), f.array.at(const_cast<void*>(b), i)))
                        return false;
                }
                return true;
            }
        }
        return false;
    }

    Json structToJson(const StructDesc& desc, const void* owner, bool omitDefaults);

    Json fieldToJson(const FieldDesc& f, const void* v){
        switch (f.type){
            case FieldType::Bool: return Json(*(const bool*)v);
            case FieldType::Int: return Json(*(const int*)v);
            case FieldType::UInt:
            case FieldType::Mask: return Json(*(const uint32_t*)v);
            case FieldType::Float: return jf(*(const float*)v);
            case FieldType::Enum: {
                const EnumOption* o = f.findOption(*(const uint8_t*)v);
                return Json(o ? o->json : f.options[0].json);
            }
            case FieldType::Vector2: { const Vector2& x = *(const Vector2*)v; return Json::array({jf(x.x), jf(x.y)}); }
            case FieldType::Vector3: { const Vector3& x = *(const Vector3*)v; return Json::array({jf(x.x), jf(x.y), jf(x.z)}); }
            case FieldType::Vector4:
            case FieldType::Color: return colorToJson(*(const Vector4*)v);
            case FieldType::String: return Json(*(const std::string*)v);
            case FieldType::MinMaxCurve: return curveValueToJson(*(const MinMaxCurve*)v);
            case FieldType::MinMaxGradient: return gradientValueToJson(*(const MinMaxGradient*)v);
            case FieldType::Curve: return curveToJson(*(const Curve*)v);
            case FieldType::Gradient: return gradientToJson(*(const Gradient*)v);
            case FieldType::Struct: return structToJson(*f.element, v, true);
            case FieldType::IntArray: return Json(*(const std::vector<int>*)v);
            case FieldType::StringArray: return Json(*(const std::vector<std::string>*)v);
            case FieldType::Array: {
                Json list = Json::array();
                const size_t n = f.array.size(v);
                for (size_t i = 0; i < n; i++){
                    list.push_back(structToJson(*f.element, f.array.at(const_cast<void*>(v), i), true));
                }
                return list;
            }
        }
        return Json();
    }

    Json structToJson(const StructDesc& desc, const void* owner, bool omitDefaults){
        Json j = Json::object();
        for (const FieldDesc& f : desc.fields){
            const void* v = f.field(owner);
            if (omitDefaults && desc.defaults && fieldEquals(f, v, f.field(desc.defaults)))
                continue;
            j[f.key] = fieldToJson(f, v);
        }
        return j;
    }

    bool structFromJson(const StructDesc& desc, const Json& j, void* owner, const std::string& path, std::vector<Diagnostic>& out,
                        const std::set<std::string>* skip = nullptr);

    bool fieldFromJson(const FieldDesc& f, const Json& j, void* v, const std::string& path, std::vector<Diagnostic>& out){
        switch (f.type){
            case FieldType::Bool:
                if (!j.is_boolean()){ error(out, path, "true or false"); return false; }
                *(bool*)v = j.get<bool>();
                return true;
            case FieldType::Int:
                if (!j.is_number_integer()){ error(out, path, "a whole number"); return false; }
                *(int*)v = j.get<int>();
                return true;
            case FieldType::UInt:
            case FieldType::Mask:
                if (!j.is_number_unsigned() && !(j.is_number_integer() && j.get<long long>() >= 0)){ error(out, path, "a whole number, 0 or more"); return false; }
                *(uint32_t*)v = j.get<uint32_t>();
                return true;
            case FieldType::Float:
                if (!readFloat(j, *(float*)v)){ error(out, path, "a finite number"); return false; }
                return true;
            case FieldType::Enum: {
                const EnumOption* o = j.is_string() ? f.findOption(j.get<std::string>()) : nullptr;
                if (!o){ error(out, path, "one of " + optionList(f)); return false; }
                *(uint8_t*)v = o->value;
                return true;
            }
            case FieldType::Vector2: {
                float x[2];
                if (!readFloats(j, x)){ error(out, path, "[x, y]"); return false; }
                *(Vector2*)v = Vector2(x[0], x[1]);
                return true;
            }
            case FieldType::Vector3: {
                float x[3];
                if (!readFloats(j, x)){ error(out, path, "[x, y, z]"); return false; }
                *(Vector3*)v = Vector3(x[0], x[1], x[2]);
                return true;
            }
            case FieldType::Vector4:
            case FieldType::Color:
                if (!colorFromJson(j, *(Vector4*)v)){ error(out, path, f.type == FieldType::Color ? "[r, g, b] or [r, g, b, a] (sRGB)" : "[x, y, z, w]"); return false; }
                return true;
            case FieldType::String:
                if (!j.is_string()){ error(out, path, "a string"); return false; }
                *(std::string*)v = j.get<std::string>();
                return true;
            case FieldType::MinMaxCurve: return curveValueFromJson(j, *(MinMaxCurve*)v, path, out);
            case FieldType::MinMaxGradient: return gradientValueFromJson(j, *(MinMaxGradient*)v, path, out);
            case FieldType::Curve: return curveFromJson(j, *(Curve*)v, path, out);
            case FieldType::Gradient: return gradientFromJson(j, *(Gradient*)v, path, out);
            case FieldType::Struct:
                if (!j.is_object()){ error(out, path, "an object"); return false; }
                return structFromJson(*f.element, j, v, path, out);
            case FieldType::IntArray: {
                if (!j.is_array()){ error(out, path, "an array of whole numbers"); return false; }
                std::vector<int>& list = *(std::vector<int>*)v;
                list.clear();
                for (size_t i = 0; i < j.size(); i++){
                    if (!j[i].is_number_integer()){ error(out, indexed(path, i), "a whole number"); return false; }
                    list.push_back(j[i].get<int>());
                }
                return true;
            }
            case FieldType::StringArray: {
                if (!j.is_array()){ error(out, path, "an array of strings"); return false; }
                std::vector<std::string>& list = *(std::vector<std::string>*)v;
                list.clear();
                for (size_t i = 0; i < j.size(); i++){
                    if (!j[i].is_string()){ error(out, indexed(path, i), "a string"); return false; }
                    list.push_back(j[i].get<std::string>());
                }
                return true;
            }
            case FieldType::Array: {
                if (!j.is_array()){ error(out, path, "an array"); return false; }
                f.array.resize(v, 0);
                f.array.resize(v, j.size());
                bool ok = true;
                for (size_t i = 0; i < j.size(); i++){
                    if (!j[i].is_object()){ error(out, indexed(path, i), "an object"); ok = false; continue; }
                    ok = structFromJson(*f.element, j[i], f.array.at(v, i), indexed(path, i), out) && ok;
                }
                return ok;
            }
        }
        return false;
    }

    bool structFromJson(const StructDesc& desc, const Json& j, void* owner, const std::string& path, std::vector<Diagnostic>& out,
                        const std::set<std::string>* skip){
        bool ok = true;
        for (auto it = j.begin(); it != j.end(); ++it){
            if (skip && skip->count(it.key()))
                continue;
            const FieldDesc* f = desc.find(it.key());
            if (!f){
                std::vector<std::string> known;
                for (const FieldDesc& field : desc.fields) known.push_back(field.key);
                if (skip) known.insert(known.end(), skip->begin(), skip->end());
                warnUnknownKey(out, join(path, it.key()), it.key(), std::string("unknown field of ") + desc.name, known);
                continue;
            }
            ok = fieldFromJson(*f, *it, f->field(owner), join(path, it.key()), out) && ok;
        }
        return ok;
    }

    // ---------------------------------------------------------------- validation

    void validateCurve(const Curve& curve, const std::string& path, bool unitTime, std::vector<Diagnostic>& out){
        for (size_t i = 0; i < curve.keys.size(); i++){
            const CurveKey& k = curve.keys[i];
            if (i > 0 && k.time < curve.keys[i - 1].time)
                error(out, indexed(path, i), "keys are sorted by time");
            if (unitTime && (k.time < 0.0f || k.time > 1.0f))
                error(out, indexed(path, i), "key times are within 0 and 1");
        }
    }

    void validateGradient(const Gradient& g, const std::string& path, std::vector<Diagnostic>& out){
        if (g.colorKeys.size() > Gradient::kMaxKeys)
            error(out, join(path, "colorKeys"), "at most 8 colour keys");
        if (g.alphaKeys.size() > Gradient::kMaxKeys)
            error(out, join(path, "alphaKeys"), "at most 8 alpha keys");
        for (size_t i = 0; i < g.colorKeys.size(); i++){
            if (g.colorKeys[i].time < 0.0f || g.colorKeys[i].time > 1.0f)
                error(out, indexed(join(path, "colorKeys"), i), "key times are within 0 and 1");
            else if (i > 0 && g.colorKeys[i].time < g.colorKeys[i - 1].time)
                error(out, indexed(join(path, "colorKeys"), i), "keys are sorted by time");
        }
        for (size_t i = 0; i < g.alphaKeys.size(); i++){
            if (g.alphaKeys[i].time < 0.0f || g.alphaKeys[i].time > 1.0f)
                error(out, indexed(join(path, "alphaKeys"), i), "key times are within 0 and 1");
            else if (i > 0 && g.alphaKeys[i].time < g.alphaKeys[i - 1].time)
                error(out, indexed(join(path, "alphaKeys"), i), "keys are sorted by time");
        }
    }

    void validateStruct(const StructDesc& desc, const void* owner, const std::string& path, std::vector<Diagnostic>& out);

    void validateField(const FieldDesc& f, const void* owner, const std::string& path, std::vector<Diagnostic>& out){
        if (f.visible && !f.visible(owner))
            return;
        const void* v = f.field(owner);
        auto outOfRange = [&](float value){ return value < f.minValue || value > f.maxValue; };
        auto rangeText = [&](){
            char text[96];
            std::snprintf(text, sizeof(text), "within %g and %g", (double)f.minValue, (double)f.maxValue);
            return std::string(text);
        };
        switch (f.type){
            case FieldType::Int:
                if (outOfRange((float)*(const int*)v)) error(out, path, rangeText());
                break;
            case FieldType::UInt:
                if (outOfRange((float)*(const uint32_t*)v)) error(out, path, rangeText());
                break;
            case FieldType::Float:
                if (outOfRange(*(const float*)v)) error(out, path, rangeText());
                break;
            case FieldType::Enum:
                if (!f.findOption(*(const uint8_t*)v)) error(out, path, "one of " + optionList(f));
                break;
            case FieldType::MinMaxCurve: {
                const MinMaxCurve& c = *(const MinMaxCurve*)v;
                if (c.usesCurve()){
                    if (c.curve.empty())
                        error(out, join(path, "curve"), "a curve mode needs at least one key");
                    validateCurve(c.curve, join(path, "curve"), true, out);
                    if (c.mode == CurveMode::TwoCurves){
                        if (c.curveMax.empty())
                            error(out, join(path, "curveMax"), "Random Between Two Curves needs a second curve");
                        validateCurve(c.curveMax, join(path, "curveMax"), true, out);
                    }
                }
                float lo, hi;
                c.range(lo, hi);
                if (lo < f.minValue || hi > f.maxValue)
                    error(out, path, "its values are " + rangeText());
                if (c.driverMin == c.driverMax && (c.driver == CurveDriver::Speed || c.driver == CurveDriver::Age))
                    error(out, join(path, "driverMax"), "the driver range is empty (driverMin equals driverMax)");
                break;
            }
            case FieldType::MinMaxGradient: {
                const MinMaxGradient& g = *(const MinMaxGradient*)v;
                if (g.usesGradient()){
                    validateGradient(g.gradient, join(path, "gradient"), out);
                    if (g.mode == GradientValueMode::TwoGradients)
                        validateGradient(g.gradientMax, join(path, "gradientMax"), out);
                }
                break;
            }
            case FieldType::Curve:
                validateCurve(*(const Curve*)v, path, false, out);
                break;
            case FieldType::Gradient:
                validateGradient(*(const Gradient*)v, path, out);
                break;
            case FieldType::Struct:
                validateStruct(*f.element, v, path, out);
                break;
            case FieldType::Array: {
                const size_t n = f.array.size(v);
                if (f.maxCount && n > f.maxCount)
                    error(out, path, "at most " + std::to_string(f.maxCount) + " entries");
                for (size_t i = 0; i < n; i++)
                    validateStruct(*f.element, f.array.at(const_cast<void*>(v), i), indexed(path, i), out);
                break;
            }
            case FieldType::IntArray:
            case FieldType::StringArray: {
                const size_t n = f.array.size(v);
                if (f.maxCount && n > f.maxCount)
                    error(out, path, "at most " + std::to_string(f.maxCount) + " entries");
                break;
            }
            default:
                break;
        }
    }

    void validateStruct(const StructDesc& desc, const void* owner, const std::string& path, std::vector<Diagnostic>& out){
        for (const FieldDesc& f : desc.fields)
            validateField(f, owner, join(path, f.key), out);
    }

    bool exposedTypeFits(ExposedType type, const FieldDesc& f, const std::string& member){
        switch (type){
            case ExposedType::Float:
                return (f.type == FieldType::Float) || (f.type == FieldType::MinMaxCurve && (member.empty() || member == "constant" || member == "constantMax" || member == "multiplier")) ||
                       ((f.type == FieldType::Vector3 || f.type == FieldType::Vector4) && (member == "x" || member == "y" || member == "z" || member == "w"));
            case ExposedType::Vector3:
                return f.type == FieldType::Vector3 && member.empty();
            case ExposedType::Color:
                return (f.type == FieldType::Color && member.empty()) || (f.type == FieldType::MinMaxGradient && (member.empty() || member == "color" || member == "colorMax"));
        }
        return false;
    }

    void validateMesh(const MeshSource& mesh, const std::string& path, std::vector<Diagnostic>& out){
        if (mesh.type == MeshSourceType::Model && mesh.model.empty())
            error(out, join(path, "model"), "a Model mesh names a model asset (.gltf, .glb or .obj)");
    }

}

Json ParticleEffectCodec::minMaxCurveToJson(const MinMaxCurve& value){
    return curveValueToJson(value);
}

bool ParticleEffectCodec::minMaxCurveFromJson(const Json& json, MinMaxCurve& value, std::vector<Diagnostic>& diagnostics){
    return curveValueFromJson(json, value, "", diagnostics);
}

Json ParticleEffectCodec::minMaxGradientToJson(const MinMaxGradient& value){
    return gradientValueToJson(value);
}

bool ParticleEffectCodec::minMaxGradientFromJson(const Json& json, MinMaxGradient& value, std::vector<Diagnostic>& diagnostics){
    return gradientValueFromJson(json, value, "", diagnostics);
}

double ParticleEffectCodec::jsonFloat(float value){
    return curvejson::jsonFloat(value);
}

Json ParticleEffectCodec::toJson(const ParticleEffectDocument& document){
    Json j = Json::object();
    j["version"] = document.version;
    const Json fields = structToJson(ParticleModules::documentFields(), &document, true);
    for (auto it = fields.begin(); it != fields.end(); ++it)
        j[it.key()] = *it;

    Json modules = Json::array();
    for (const ModuleDesc& m : ParticleModules::all()){
        if (!m.isPresent(document))
            continue;
        Json entry = Json::object();
        entry["type"] = m.key;
        if (m.hasEnable && !m.isEnabled(document))
            entry["enabled"] = false;
        const Json body = structToJson(m.fields, m.module(document), true);
        for (auto it = body.begin(); it != body.end(); ++it)
            entry[it.key()] = *it;
        modules.push_back(entry);
    }
    j["modules"] = modules;

    if (!document.subEmitters.empty()){
        Json list = Json::array();
        for (const SubEmitterEntry& e : document.subEmitters)
            list.push_back(structToJson(ParticleModules::subEmitterFields(), &e, true));
        j["subEmitters"] = list;
    }
    if (!document.exposed.empty()){
        Json list = Json::array();
        for (const ExposedParameter& e : document.exposed)
            list.push_back(structToJson(ParticleModules::exposedFields(), &e, false));
        j["exposed"] = list;
    }
    return j;
}

std::string ParticleEffectCodec::write(const ParticleEffectDocument& document){
    return toJson(document).dump(2) + "\n";
}

bool ParticleEffectCodec::fromJson(const Json& json, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics){
    const size_t firstDiagnostic = diagnostics.size();
    document = ParticleEffectDocument();
    if (!json.is_object()){
        error(diagnostics, "$", "a particle effect is a JSON object");
        return false;
    }
    bool ok = true;
    if (!json.contains("version")){
        error(diagnostics, "version", "the schema version is missing (1)");
        ok = false;
    }else if (!json["version"].is_number_integer()){
        error(diagnostics, "version", "the schema version is a whole number (1)");
        ok = false;
    }else{
        document.version = json["version"].get<int>();
        if (document.version > kEffectSchemaVersion){
            error(diagnostics, "version", "written by a newer JasperEngine (schema " + std::to_string(document.version) + "; this one reads 1)");
            ok = false;
        }else if (document.version < 1){
            error(diagnostics, "version", "unknown schema version " + std::to_string(document.version));
            ok = false;
        }
    }

    static const std::set<std::string> reserved = {"version", "modules", "subEmitters", "exposed"};
    ok = structFromJson(ParticleModules::documentFields(), json, &document, "", diagnostics, &reserved) && ok;

    // modules: hidden unless listed (Main and Renderer always present)
    for (const ModuleDesc& m : ParticleModules::all())
        m.setPresent(document, false);
    if (json.contains("modules")){
        const Json& modules = json["modules"];
        if (!modules.is_array()){
            error(diagnostics, "modules", "an array of {type, enabled, ...fields}");
            ok = false;
        }else{
            std::set<std::string> seen;
            for (size_t i = 0; i < modules.size(); i++){
                const Json& entry = modules[i];
                const std::string at = indexed("modules", i);
                if (!entry.is_object() || !entry.contains("type") || !entry["type"].is_string()){
                    error(diagnostics, at, "a module is an object with a \"type\"");
                    ok = false;
                    continue;
                }
                const std::string type = entry["type"].get<std::string>();
                const ModuleDesc* m = ParticleModules::find(type);
                if (!m){
                    error(diagnostics, at + ".type", "unknown module \"" + type + "\"");
                    ok = false;
                    continue;
                }
                if (!seen.insert(type).second){
                    error(diagnostics, at + ".type", "the module \"" + type + "\" is listed twice");
                    ok = false;
                    continue;
                }
                m->setPresent(document, true);
                if (entry.contains("enabled")){
                    if (!entry["enabled"].is_boolean() || !m->hasEnable){
                        error(diagnostics, std::string(m->key) + ".enabled", m->hasEnable ? "true or false" : "this module is always on");
                        ok = false;
                    }else{
                        m->setEnabled(document, entry["enabled"].get<bool>());
                    }
                }else{
                    m->setEnabled(document, true);
                }
                // the keys of older files that a field replaced: Velocity over Lifetime's offset vector
                // (now the Orbital Offset curves), the Renderer's Align Z to Velocity (now a Render
                // Alignment); a file that has both keys takes the new one
                static const std::set<std::string> header = {"type", "enabled", "offset", "alignToVelocity"};
                ok = structFromJson(m->fields, entry, m->module(document), m->key, diagnostics, &header) && ok;
                if (m->id == ModuleId::VelocityOverLifetime && entry.contains("offset")){
                    float v[3];
                    if (!readFloats(entry["offset"], v)){
                        error(diagnostics, std::string(m->key) + ".offset", "an offset is [x, y, z]");
                        ok = false;
                    }else{
                        VelocityOverLifetimeModule& vol = document.velocityOverLifetime;
                        if (!entry.contains("offsetX")) vol.offsetX = MinMaxCurve(v[0]);
                        if (!entry.contains("offsetY")) vol.offsetY = MinMaxCurve(v[1]);
                        if (!entry.contains("offsetZ")) vol.offsetZ = MinMaxCurve(v[2]);
                    }
                }
                if (m->id == ModuleId::Renderer && entry.contains("alignToVelocity")){
                    if (!entry["alignToVelocity"].is_boolean()){
                        error(diagnostics, std::string(m->key) + ".alignToVelocity", "true or false");
                        ok = false;
                    }else if (entry["alignToVelocity"].get<bool>() && !entry.contains("meshAlignment")){
                        document.renderer.meshAlignment = MeshAlignment::Velocity;
                    }
                }
            }
        }
    }

    auto readList = [&](const char* key, const StructDesc& desc, auto& list){
        if (!json.contains(key))
            return;
        const Json& array = json[key];
        if (!array.is_array()){
            error(diagnostics, key, "an array");
            ok = false;
            return;
        }
        list.resize(array.size());
        for (size_t i = 0; i < array.size(); i++){
            if (!array[i].is_object()){
                error(diagnostics, indexed(key, i), "an object");
                ok = false;
                continue;
            }
            ok = structFromJson(desc, array[i], &list[i], indexed(key, i), diagnostics) && ok;
        }
    };
    readList("subEmitters", ParticleModules::subEmitterFields(), document.subEmitters);
    readList("exposed", ParticleModules::exposedFields(), document.exposed);

    ok = validate(document, diagnostics) && ok;
    for (size_t i = firstDiagnostic; i < diagnostics.size(); i++){
        if (diagnostics[i].severity == Diagnostic::Severity::Error)
            ok = false;
    }
    return ok;
}

bool jasperengine::particles::readEffectText(const std::string& text, ParticleEffectDocument& document){
    std::vector<Diagnostic> diagnostics;
    return ParticleEffectCodec::read(text, document, diagnostics);
}

bool ParticleEffectCodec::read(std::string_view text, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics){
    Json json;
    try {
        json = Json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::parse_error& e) {
        document = ParticleEffectDocument();
        error(diagnostics, "$", std::string("JSON syntax: ") + e.what());
        return false;
    }
    return fromJson(json, document, diagnostics);
}

bool ParticleEffectCodec::validate(const ParticleEffectDocument& d, std::vector<Diagnostic>& out){
    const size_t first = out.size();
    validateStruct(ParticleModules::documentFields(), &d, "", out);
    for (const ModuleDesc& m : ParticleModules::all()){
        if (m.isPresent(d))
            validateStruct(m.fields, m.module(d), m.key, out);
    }
    for (size_t i = 0; i < d.subEmitters.size(); i++)
        validateStruct(ParticleModules::subEmitterFields(), &d.subEmitters[i], indexed("subEmitters", i), out);

    // what the field ranges do not say
    if (d.startDelay.usesCurve())
        error(out, "startDelay", "a start delay is a constant or a random between two constants");
    if (d.shape.present && d.shape.enabled){
        if (d.shape.shape == ShapeType::Mesh)
            validateMesh(d.shape.mesh, "shape.mesh", out);
        if (d.shape.shape == ShapeType::Cone && d.shape.angle >= 90.0f)
            error(out, "shape.angle", "a cone's angle is below 90 degrees");
    }
    if (d.textureSheetAnimation.present && d.textureSheetAnimation.enabled){
        const TextureSheetAnimationModule& t = d.textureSheetAnimation;
        if (t.mode == TextureSheetMode::Sprites && t.sprites.empty())
            error(out, "textureSheetAnimation.sprites", "Sprites mode lists at least one sprite");
        if (t.mode == TextureSheetMode::Grid && t.animation == TextureSheetAnimation::SingleRow &&
                t.rowMode == TextureSheetRowMode::Custom && t.rowIndex >= t.tilesY)
            error(out, "textureSheetAnimation.rowIndex", "the row is below Tiles Y (" + std::to_string(t.tilesY) + ")");
        const int frames = t.mode == TextureSheetMode::Grid ? t.tilesX * t.tilesY : (int)t.sprites.size();
        for (size_t i = 0; i < t.startFrameList.size(); i++){
            if (t.startFrameList[i] < 0 || t.startFrameList[i] >= std::max(frames, 1))
                error(out, indexed("textureSheetAnimation.startFrameList", i), "a frame of the sheet (0 to " + std::to_string(std::max(frames, 1) - 1) + ")");
        }
    }
    if (d.collision.present && d.collision.enabled){
        for (size_t i = 0; i < d.collision.planes.size(); i++){
            const Vector3& n = d.collision.planes[i].normal;
            if (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f)
                error(out, indexed("collision.planes", i) + ".normal", "a plane's normal is not zero");
        }
        if (d.collision.minKillSpeed > d.collision.maxKillSpeed)
            error(out, "collision.minKillSpeed", "Min Kill Speed is not above Max Kill Speed");
    }
    if (d.orbit.present && d.orbit.enabled){
        const Vector3& a = d.orbit.axis;
        if (a.x == 0.0f && a.y == 0.0f && a.z == 0.0f)
            error(out, "orbit.axis", "the orbit axis is not zero");
    }
    if (d.renderer.enabled){
        const RendererModule& r = d.renderer;
        if (r.renderMode == RenderMode::Mesh){
            if (r.meshes.empty())
                error(out, "renderer.meshes", "the Mesh render mode needs 1 to 4 meshes");
            for (size_t i = 0; i < r.meshes.size(); i++)
                validateMesh(r.meshes[i], indexed("renderer.meshes", i), out);
        }
        if (r.lighting == LightingMode::SixWay && (r.sixWayMapA.empty() || r.sixWayMapB.empty()))
            error(out, r.sixWayMapA.empty() ? "renderer.sixWayMapA" : "renderer.sixWayMapB", "Six-Way lighting needs both lightmaps");
        if (r.minScreenSize > r.maxScreenSize)
            error(out, "renderer.minScreenSize", "Min Particle Size is not above Max Particle Size");
        if (r.shader.empty())
            error(out, "renderer.shader", "a shader (library:particles, or a fork of it)");
        if (r.renderMode == RenderMode::None && !(d.trails.present && d.trails.enabled))
            warn(out, "renderer.renderMode", "None draws only trails, and Trails is off");
    }
    if (d.setOverAge.present && d.setOverAge.enabled){
        for (size_t i = 0; i < d.setOverAge.entries.size(); i++){
            const SetOverAgeEntry& e = d.setOverAge.entries[i];
            if (e.property == SetProperty::Frame && e.frames.empty())
                error(out, indexed("setOverAge.entries", i) + ".frames", "a Frame entry lists its frames");
        }
    }
    if (d.subEmitters.size() > kMaxSubEmitters)
        error(out, "subEmitters", "at most 8 sub-emitters");
    for (size_t i = 0; i < d.subEmitters.size(); i++){
        const SubEmitterEntry& e = d.subEmitters[i];
        if (e.effect.empty())
            error(out, indexed("subEmitters", i) + ".effect", "a sub-emitter names a .particles effect");
        if (e.trigger == SubEmitterTrigger::Trigger && e.triggerName.empty())
            error(out, indexed("subEmitters", i) + ".triggerName", "a Trigger sub-emitter has the name sendTrigger() raises");
        if (e.trigger == SubEmitterTrigger::Collision && !(d.collision.present && d.collision.enabled))
            warn(out, indexed("subEmitters", i) + ".trigger", "Collision sub-emitters need the Collision module");
    }
    if (!d.subEmitters.empty() && !(d.subEmittersModule.present && d.subEmittersModule.enabled))
        warn(out, "subEmitters", "the Sub Emitters module is off: the list does nothing");

    if (d.exposed.size() > kMaxExposed)
        error(out, "exposed", "at most 32 exposed parameters");
    std::set<std::string> names;
    for (size_t i = 0; i < d.exposed.size(); i++){
        const ExposedParameter& e = d.exposed[i];
        const std::string at = indexed("exposed", i);
        if (e.name.empty())
            error(out, at + ".name", "an exposed parameter has a name");
        else if (!names.insert(e.name).second)
            error(out, at + ".name", "the name \"" + e.name + "\" is used twice");
        ParticleEffectDocument copy = d;
        const ParticleModules::Resolved r = ParticleModules::resolve(copy, e.path);
        if (!r.field)
            error(out, at + ".path", "\"" + e.path + "\" is not a field (module.field, e.g. emission.rateOverTime)");
        else if (!exposedTypeFits(e.type, *r.field, r.member))
            error(out, at + ".type", "the field \"" + e.path + "\" does not hold that type");
    }

    for (size_t i = first; i < out.size(); i++){
        if (out[i].severity == Diagnostic::Severity::Error)
            return false;
    }
    return true;
}

Json ParticleEffectCodec::toKeyedJson(const ParticleEffectDocument& document){
    Json j = toJson(document);
    Json keyed = Json::object();
    for (const Json& entry : j["modules"]){
        Json body = entry;
        const std::string type = body["type"].get<std::string>();
        body.erase("type");
        keyed[type] = body;
    }
    j["modules"] = keyed;
    return j;
}

namespace {

    void mergePatch(Json& target, const Json& patch){
        if (!patch.is_object()){
            target = patch;
            return;
        }
        if (!target.is_object())
            target = Json::object();
        // a gradient has two object forms and the keyed form holds {colorKeys, alphaKeys}: a patch
        // in the other form replaces them, or the reader would meet them as unknown keys
        if ((patch.contains("colors") || patch.contains("alphas")) && (target.contains("colorKeys") || target.contains("alphaKeys"))){
            target.erase("colorKeys");
            target.erase("alphaKeys");
            target.erase("linear");
        }
        for (auto it = patch.begin(); it != patch.end(); ++it){
            if (it->is_null()){
                target.erase(it.key());
            }else{
                mergePatch(target[it.key()], *it);
            }
        }
    }

}

bool ParticleEffectCodec::applyMergePatch(ParticleEffectDocument& document, const Json& patch, std::vector<Diagnostic>& diagnostics){
    if (!patch.is_object()){
        error(diagnostics, "$", "a patch is a JSON object (RFC 7386 merge patch of the effect, modules keyed by type)");
        return false;
    }
    if (patch.contains("modules") && !patch["modules"].is_object() && !patch["modules"].is_null()){
        error(diagnostics, "modules", "in a patch, modules is an object keyed by module type: {\"emission\": {\"rateOverTime\": 20}}");
        return false;
    }
    Json keyed = toKeyedJson(document);
    mergePatch(keyed, patch);

    // back to the stored form: modules as an array in the registry's order
    Json modules = Json::array();
    const Json& keyedModules = keyed.contains("modules") && keyed["modules"].is_object() ? keyed["modules"] : Json::object();
    for (auto it = keyedModules.begin(); it != keyedModules.end(); ++it){
        if (!ParticleModules::find(it.key())){
            std::vector<std::string> known;
            for (const ModuleDesc& m : ParticleModules::all()) known.push_back(m.key);
            error(diagnostics, "modules." + it.key(), "unknown module \"" + it.key() + "\"" + NearestNameSuffix(it.key(), known));
            return false;
        }
        if (!it->is_object()){
            error(diagnostics, "modules." + it.key(), "a module is an object of its fields");
            return false;
        }
    }
    for (const ModuleDesc& m : ParticleModules::all()){
        if (!keyedModules.contains(m.key)){
            if (m.alwaysPresent){
                // Main and Renderer cannot be removed: reset to their defaults
                Json entry = Json::object();
                entry["type"] = m.key;
                modules.push_back(entry);
            }
            continue;
        }
        Json entry = Json::object();
        entry["type"] = m.key;
        const Json& body = keyedModules[m.key];
        for (auto it = body.begin(); it != body.end(); ++it)
            entry[it.key()] = *it;
        modules.push_back(entry);
    }
    keyed["modules"] = modules;

    // The keyed form holds only what the format has, so an unknown key can only be the patch's: a
    // misspelled field would be dropped without a trace, so it is an error here, not a warning
    ParticleEffectDocument patched;
    const size_t firstDiagnostic = diagnostics.size();
    bool ok = fromJson(keyed, patched, diagnostics);
    for (size_t i = firstDiagnostic; i < diagnostics.size(); i++){
        if (!diagnostics[i].unknownKey)
            continue;
        diagnostics[i].severity = Diagnostic::Severity::Error;
        ok = false;
    }
    if (!ok)
        return false;
    document = std::move(patched);
    return true;
}

std::string ParticleEffectCodec::format(const std::vector<Diagnostic>& diagnostics){
    std::string text;
    for (const Diagnostic& d : diagnostics){
        if (!text.empty()) text += "\n";
        text += (d.severity == Diagnostic::Severity::Error ? "error: " : "warning: ") + d.path + ": " + d.message;
    }
    return text;
}

bool ParticleEffectCodec::hasErrors(const std::vector<Diagnostic>& diagnostics){
    for (const Diagnostic& d : diagnostics){
        if (d.severity == Diagnostic::Severity::Error)
            return true;
    }
    return false;
}
