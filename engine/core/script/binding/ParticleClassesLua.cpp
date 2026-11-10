// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Particle Systems' module proxies in Lua (docs/specs/S20-particles.md, Upstream absorption):
// Unity's ps.main, ps.emission, ps.shape, ... (ParticleSystemModule), each field of S20's module
// table (particles/ParticleModules) by its Unity name: numbers, booleans, vectors, MinMaxCurve /
// MinMaxGradient values, and enum values as their names (the ParticleSystem* tables hold them).
// Rotations and angular speeds read and take radians, as Unity's do (the effect stores degrees).
// A write makes the effect the system's own (ParticleSystem::setEffectDocument). Also Unity's
// play / stop / pause / clear with children, and the ParticleSystemStopBehavior values. The
// ParticleSystem class itself is in GameObjectClassesLua.cpp.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/ParticleSystem.h"
#include "particles/AnalyticParticles.h"
#include "particles/ParticleModules.h"
#include "subsystem/ParticleSystemState.h"

#include <cmath>
#include <optional>
#include <string>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kRadToDeg = 57.29577951308232f;

    // a module of one Particle System (ps.main): the GameObject, the module's key
    struct ParticleSystemModule {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::string module;
    };

    // a colour field of a module (ps.colorOverLifetime.color): reads and writes go to the effect
    // now, so :setRamp() on it changes the system (S49 R40); it answers as a MinMaxGradient does
    struct ParticleSystemGradient {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::string path;    // "colorOverLifetime.color"
    };

    bool gradientOf(const ParticleSystemGradient& view, MinMaxGradient& out);

    ParticleSystem systemOf(const ParticleSystemModule& m){
        return ParticleSystem(m.scene, m.entity);
    }

    bool alive(const ParticleSystemModule& m){
        return m.scene && m.scene->isEntityCreated(m.entity) && m.scene->findComponent<ParticleSystemComponent>(m.entity);
    }

    // Unity's module and field names that are other paths here: the separate Inherit Velocity
    // module (Main's fields), the Unity spellings of a few fields ("startSizeX", a size module's
    // "x", Limit Velocity's "limitX")
    std::string pathOf(const std::string& module, const std::string& name){
        if (module == "inheritVelocity"){
            if (name == "curve") return "main.inheritVelocity";
            if (name == "mode") return "main.inheritVelocityMode";
        }
        if (module == "main"){
            if (name == "startSizeX") return "main.startSize";
            if (name == "startRotationZ") return "main.startRotation";
            if (name == "duration" || name == "looping" || name == "prewarm" || name == "startDelay" || name == "maxParticles" ||
                name == "simulationSpace" || name == "simulationSpeed" || name == "scalingMode" || name == "useUnscaledTime" ||
                name == "stopAction" || name == "emitterVelocityMode")
                return name;   // the effect's own fields (Unity puts them in Main)
            if (name == "randomSeed") return "seed";
        }
        if (module == "noise" && name == "strengthX") return "noise.strength";
        if ((module == "sizeOverLifetime" || module == "sizeBySpeed") && name == "x") return module + ".size";
        if ((module == "sizeOverLifetime" || module == "sizeBySpeed") && (name == "y" || name == "z")) return module + ".size" + (name == "y" ? "Y" : "Z");
        if ((module == "rotationOverLifetime" || module == "rotationBySpeed") && name == "z") return module + ".angularVelocity";
        if ((module == "rotationOverLifetime" || module == "rotationBySpeed") && (name == "x" || name == "y"))
            return module + ".angularVelocity" + (name == "x" ? "X" : "Y");
        if (module == "limitVelocity" && name == "limitX") return "limitVelocity.limit";
        if (module == "textureSheetAnimation" && (name == "numTilesX" || name == "numTilesY")) return module + ".tiles" + (name == "numTilesX" ? "X" : "Y");
        if (module == "velocityOverLifetime" && (name == "orbitalOffsetX" || name == "orbitalOffsetY" || name == "orbitalOffsetZ"))
            return module + ".offset" + name.substr(name.size() - 1);
        return module + "." + name;
    }

    // the module key of a proxy name (Unity's limitVelocityOverLifetime is limitVelocity)
    std::string moduleKey(const std::string& name){
        if (name == "limitVelocityOverLifetime") return "limitVelocity";
        return name;
    }

    luabridge::LuaRef toLua(lua_State* L, const FieldDesc& f, const void* v){
        const bool degrees = (f.flags & FieldFlag_Degrees) != 0;
        switch (f.type){
            case FieldType::Bool: return luabridge::LuaRef(L, *(const bool*)v);
            case FieldType::Int: return luabridge::LuaRef(L, *(const int*)v);
            case FieldType::UInt:
            case FieldType::Mask: return luabridge::LuaRef(L, *(const uint32_t*)v);
            case FieldType::Float: return luabridge::LuaRef(L, degrees ? *(const float*)v / kRadToDeg : *(const float*)v);
            case FieldType::Enum: {
                const EnumOption* option = f.findOption(*(const uint8_t*)v);
                return option ? luabridge::LuaRef(L, std::string(option->json)) : luabridge::LuaRef(L);
            }
            case FieldType::Vector2: return luabridge::LuaRef(L, *(const Vector2*)v);
            case FieldType::Vector3: {
                Vector3 value = *(const Vector3*)v;
                if (degrees) value = value / kRadToDeg;
                return luabridge::LuaRef(L, value);
            }
            case FieldType::Vector4:
            case FieldType::Color: return luabridge::LuaRef(L, *(const Vector4*)v);
            case FieldType::String: return luabridge::LuaRef(L, *(const std::string*)v);
            case FieldType::MinMaxCurve: {
                MinMaxCurve value = *(const MinMaxCurve*)v;
                if (degrees){
                    value.constant /= kRadToDeg;
                    value.constantMax /= kRadToDeg;
                    value.multiplier /= kRadToDeg;
                }
                return luabridge::LuaRef(L, value);
            }
            case FieldType::MinMaxGradient: return luabridge::LuaRef(L, *(const MinMaxGradient*)v);
            case FieldType::Curve: return luabridge::LuaRef(L, *(const Curve*)v);
            case FieldType::Gradient: return luabridge::LuaRef(L, *(const Gradient*)v);
            case FieldType::IntArray: {
                luabridge::LuaRef table = luabridge::newTable(L);
                const std::vector<int>& list = *(const std::vector<int>*)v;
                for (size_t i = 0; i < list.size(); i++) table[(int)i + 1] = list[i];
                return table;
            }
            case FieldType::StringArray: {
                luabridge::LuaRef table = luabridge::newTable(L);
                const std::vector<std::string>& list = *(const std::vector<std::string>*)v;
                for (size_t i = 0; i < list.size(); i++) table[(int)i + 1] = list[i];
                return table;
            }
            case FieldType::Struct:
            case FieldType::Array:
                break;
        }
        return luabridge::LuaRef(L);
    }

    // false (with why) for a value the field does not take
    bool fromLua(const FieldDesc& f, const luabridge::LuaRef& value, void* v, std::string& why){
        const bool degrees = (f.flags & FieldFlag_Degrees) != 0;
        switch (f.type){
            case FieldType::Bool:
                if (!value.isBool()){ why = "true or false"; return false; }
                *(bool*)v = value.unsafe_cast<bool>();
                return true;
            case FieldType::Int:
                if (!value.isNumber()){ why = "a whole number"; return false; }
                *(int*)v = (int)std::lround(std::clamp(value.unsafe_cast<double>(), (double)f.minValue, (double)f.maxValue));
                return true;
            case FieldType::UInt:
            case FieldType::Mask:
                if (!value.isNumber() || value.unsafe_cast<double>() < 0.0){ why = "a whole number from 0"; return false; }
                *(uint32_t*)v = (uint32_t)std::llround(f.type == FieldType::UInt ? std::clamp(value.unsafe_cast<double>(), (double)f.minValue, (double)f.maxValue)
                                                                                   : value.unsafe_cast<double>());
                return true;
            case FieldType::Float:
                if (!value.isNumber() || !std::isfinite(value.unsafe_cast<float>())){ why = "a number"; return false; }
                *(float*)v = std::clamp(degrees ? value.unsafe_cast<float>() * kRadToDeg : value.unsafe_cast<float>(), f.minValue, f.maxValue);
                return true;
            case FieldType::Enum: {
                const EnumOption* option = nullptr;
                if (value.isString()) option = f.findOption(value.unsafe_cast<std::string>());
                else if (value.isNumber()) option = f.findOption((uint8_t)value.unsafe_cast<int>());
                if (!option){
                    why = "one of";
                    for (uint8_t i = 0; i < f.optionCount; i++) why += std::string(i ? ", " : " ") + f.options[i].json;
                    return false;
                }
                *(uint8_t*)v = option->value;
                return true;
            }
            case FieldType::Vector2:
                if (!value.isInstance<Vector2>()){ why = "a Vector2"; return false; }
                *(Vector2*)v = value.unsafe_cast<Vector2>();
                return true;
            case FieldType::Vector3:
                if (!value.isInstance<Vector3>()){ why = "a Vector3"; return false; }
                *(Vector3*)v = degrees ? value.unsafe_cast<Vector3>() * kRadToDeg : value.unsafe_cast<Vector3>();
                return true;
            case FieldType::Vector4:
            case FieldType::Color:
                if (!value.isInstance<Vector4>()){ why = "a Vector4"; return false; }
                *(Vector4*)v = value.unsafe_cast<Vector4>();
                return true;
            case FieldType::String:
                if (!value.isString()){ why = "a string"; return false; }
                *(std::string*)v = value.unsafe_cast<std::string>();
                return true;
            case FieldType::MinMaxCurve: {
                MinMaxCurve c;
                if (value.isNumber()){
                    c = MinMaxCurve(value.unsafe_cast<float>());
                }else if (value.isInstance<MinMaxCurve>()){
                    c = value.unsafe_cast<MinMaxCurve>();
                }else if (value.isInstance<Curve>()){
                    c = MinMaxCurve::fromCurve(value.unsafe_cast<Curve>(), 1.0f);
                }else{
                    why = "a number, a MinMaxCurve or an AnimationCurve";
                    return false;
                }
                if (degrees){
                    c.constant *= kRadToDeg;
                    c.constantMax *= kRadToDeg;
                    c.multiplier *= kRadToDeg;
                }
                *(MinMaxCurve*)v = c;
                return true;
            }
            case FieldType::MinMaxGradient:
                if (value.isInstance<ParticleSystemGradient>()){
                    MinMaxGradient g;
                    if (!gradientOf(value.unsafe_cast<ParticleSystemGradient>(), g)){ why = "the colour's Particle System is gone"; return false; }
                    *(MinMaxGradient*)v = g;
                    return true;
                }
                if (value.isInstance<MinMaxGradient>()){
                    *(MinMaxGradient*)v = value.unsafe_cast<MinMaxGradient>();
                    return true;
                }
                if (value.isInstance<Vector4>()){
                    *(MinMaxGradient*)v = MinMaxGradient(value.unsafe_cast<Vector4>());
                    return true;
                }
                if (value.isInstance<Gradient>()){
                    MinMaxGradient g;
                    g.mode = GradientValueMode::Gradient;
                    g.gradient = value.unsafe_cast<Gradient>();
                    *(MinMaxGradient*)v = g;
                    return true;
                }
                why = "a MinMaxGradient, a Gradient or a Vector4 colour (sRGB)";
                return false;
            case FieldType::Curve:
                if (!value.isInstance<Curve>()){ why = "an AnimationCurve"; return false; }
                *(Curve*)v = value.unsafe_cast<Curve>();
                return true;
            case FieldType::Gradient:
                if (!value.isInstance<Gradient>()){ why = "a Gradient"; return false; }
                *(Gradient*)v = value.unsafe_cast<Gradient>();
                return true;
            case FieldType::IntArray: {
                if (!value.isTable()){ why = "a table of whole numbers"; return false; }
                std::vector<int> list;
                for (int i = 1; i <= value.length(); i++){
                    const luabridge::LuaRef e = value[i];
                    if (!e.isNumber()){ why = "a table of whole numbers"; return false; }
                    list.push_back(e.unsafe_cast<int>());
                }
                *(std::vector<int>*)v = std::move(list);
                return true;
            }
            case FieldType::StringArray: {
                if (!value.isTable()){ why = "a table of strings"; return false; }
                std::vector<std::string> list;
                for (int i = 1; i <= value.length(); i++){
                    const luabridge::LuaRef e = value[i];
                    if (!e.isString()){ why = "a table of strings"; return false; }
                    list.push_back(e.unsafe_cast<std::string>());
                }
                *(std::vector<std::string>*)v = std::move(list);
                return true;
            }
            case FieldType::Struct:
            case FieldType::Array:
                break;
        }
        why = "set with setInlineEffect (a list of structures)";
        return false;
    }

    // a burst as Unity's ParticleSystem.Burst fields: {time, count (a number or MinMaxCurve), cycles, interval, probability}
    luabridge::LuaRef burstToTable(lua_State* L, const Burst& b){
        luabridge::LuaRef t = luabridge::newTable(L);
        t["time"] = b.time;
        t["count"] = b.count;
        t["cycles"] = b.cycles;
        t["interval"] = b.interval;
        t["probability"] = b.probability;
        return t;
    }

    bool tableToBurst(const luabridge::LuaRef& t, Burst& b, std::string& why){
        if (!t.isTable()){ why = "a burst is {time, count, cycles, interval, probability}"; return false; }
        const luabridge::LuaRef time = t["time"], count = t["count"], cycles = t["cycles"], interval = t["interval"], probability = t["probability"];
        if (!time.isNil()){ if (!time.isNumber()){ why = "time is a number"; return false; } b.time = std::max(0.0f, time.unsafe_cast<float>()); }
        if (!count.isNil()){
            if (count.isNumber()) b.count = MinMaxCurve(count.unsafe_cast<float>());
            else if (count.isInstance<MinMaxCurve>()) b.count = count.unsafe_cast<MinMaxCurve>();
            else { why = "count is a number or a MinMaxCurve"; return false; }
        }
        if (!cycles.isNil()){ if (!cycles.isNumber()){ why = "cycles is a number"; return false; } b.cycles = std::max(0, cycles.unsafe_cast<int>()); }
        if (!interval.isNil()){ if (!interval.isNumber()){ why = "interval is a number"; return false; } b.interval = std::max(1.0e-4f, interval.unsafe_cast<float>()); }
        if (!probability.isNil()){ if (!probability.isNumber()){ why = "probability is a number"; return false; } b.probability = std::clamp(probability.unsafe_cast<float>(), 0.0f, 1.0f); }
        return true;
    }

    bool isMethod(const std::string& name){
        return name == "getBurst" || name == "setBurst" || name == "setBursts";
    }

    luabridge::LuaRef moduleIndex(ParticleSystemModule& self, const luabridge::LuaRef& key, lua_State* L){
        if (!key.isString()) return luabridge::LuaRef(L);
        const std::string name = key.unsafe_cast<std::string>();
        // nil for the methods (LuaBridge finds them) and the metamethods
        if (isMethod(name) || name.rfind("__", 0) == 0) return luabridge::LuaRef(L);
        if (!alive(self)) return luabridge::LuaRef(L);
        ParticleEffectDocument document = systemOf(self).getEffectDocument();
        const ModuleDesc* m = ParticleModules::find(self.module);
        if (name == "enabled" && self.module == "inheritVelocity")
            return luabridge::LuaRef(L, !document.main.inheritVelocity.isZero());
        if (name == "enabled" && m)
            return luabridge::LuaRef(L, m->isPresent(document) && m->isEnabled(document));
        if (self.module == "emission" && name == "burstCount")
            return luabridge::LuaRef(L, (int)document.emission.bursts.size());
        const std::string path = pathOf(self.module, name);
        // "<curve>Multiplier": the curve's multiplier (its constant in Constant mode)
        const std::string suffix = "Multiplier";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0){
            const ParticleModules::Resolved r = ParticleModules::resolve(document, pathOf(self.module, name.substr(0, name.size() - suffix.size())));
            if (r.field && r.field->type == FieldType::MinMaxCurve){
                const MinMaxCurve& c = *(const MinMaxCurve*)r.storage;
                const float scale = (r.field->flags & FieldFlag_Degrees) ? 1.0f / kRadToDeg : 1.0f;
                return luabridge::LuaRef(L, (c.usesCurve() ? c.multiplier : c.constant) * scale);
            }
        }
        const ParticleModules::Resolved r = ParticleModules::resolve(document, path);
        if (!r.field || !r.member.empty()){
            Log::error("ParticleSystem.%s has no field '%s' (docs/mcp/guide/particles.md lists them)", self.module.c_str(), name.c_str());
            return luabridge::LuaRef(L);
        }
        if (r.field->type == FieldType::MinMaxGradient)
            return luabridge::LuaRef(L, ParticleSystemGradient{self.scene, self.entity, path});
        return toLua(L, *r.field, r.storage);
    }

    // the colour a gradient view names, now
    bool gradientOf(const ParticleSystemGradient& view, MinMaxGradient& out){
        if (!view.scene || !view.scene->isEntityCreated(view.entity) || !view.scene->findComponent<ParticleSystemComponent>(view.entity))
            return false;
        ParticleEffectDocument document = ParticleSystem(view.scene, view.entity).getEffectDocument();
        const ParticleModules::Resolved r = ParticleModules::resolve(document, view.path);
        if (!r.field || r.field->type != FieldType::MinMaxGradient)
            return false;
        out = *(const MinMaxGradient*)r.storage;
        return true;
    }

    bool setGradientOf(const ParticleSystemGradient& view, const MinMaxGradient& value){
        if (!view.scene || !view.scene->isEntityCreated(view.entity) || !view.scene->findComponent<ParticleSystemComponent>(view.entity))
            return false;
        ParticleSystem system(view.scene, view.entity);
        ParticleEffectDocument document = system.getEffectDocument();
        const ParticleModules::Resolved r = ParticleModules::resolve(document, view.path);
        if (!r.field || r.field->type != FieldType::MinMaxGradient)
            return false;
        *(MinMaxGradient*)r.storage = value;
        if (r.module) r.module->setPresent(document, true);
        system.setEffectDocument(document);
        return true;
    }

    // MinMaxGradient:setRamp(palette, ramp[, steps]): a Palette Ramp of "<palette>#ramp:<ramp>"
    void applyRamp(MinMaxGradient& g, const std::string& palette, const std::string& ramp, int steps){
        g.mode = GradientValueMode::Ramp;
        g.ramp = palette + "#ramp:" + ramp;
        g.rampColors.clear();
        g.rampSteps = steps;
    }

    luabridge::LuaRef gradientIndex(ParticleSystemGradient& self, const luabridge::LuaRef& key, lua_State* L){
        if (!key.isString()) return luabridge::LuaRef(L);
        const std::string name = key.unsafe_cast<std::string>();
        if (name == "setRamp" || name == "evaluate" || name == "value" || name.rfind("__", 0) == 0) return luabridge::LuaRef(L);
        MinMaxGradient g;
        if (!gradientOf(self, g)) return luabridge::LuaRef(L);
        luabridge::LuaRef value(L, g);
        return value[name];
    }

    luabridge::LuaRef gradientNewIndex(ParticleSystemGradient& self, const luabridge::LuaRef& key, const luabridge::LuaRef& v, lua_State* L){
        if (!key.isString()) return luabridge::LuaRef(L);
        MinMaxGradient g;
        if (!gradientOf(self, g)) return luabridge::LuaRef(L);
        luabridge::LuaRef value(L, g);
        value[key.unsafe_cast<std::string>()] = v;
        setGradientOf(self, value.unsafe_cast<MinMaxGradient>());
        return luabridge::LuaRef(L);
    }

    luabridge::LuaRef moduleNewIndex(ParticleSystemModule& self, const luabridge::LuaRef& key, const luabridge::LuaRef& value, lua_State* L){
        if (!key.isString() || !alive(self)) return luabridge::LuaRef(L);
        const std::string name = key.unsafe_cast<std::string>();
        ParticleSystem system = systemOf(self);
        ParticleEffectDocument document = system.getEffectDocument();
        const ModuleDesc* m = ParticleModules::find(self.module);
        if (name == "enabled" && self.module == "inheritVelocity"){
            if (!value.isBool()){ Log::error("ParticleSystem.inheritVelocity.enabled: true or false"); return luabridge::LuaRef(L); }
            if (!value.unsafe_cast<bool>()) document.main.inheritVelocity = MinMaxCurve(0.0f);
            system.setEffectDocument(document);
            return luabridge::LuaRef(L);
        }
        if (name == "enabled" && m){
            if (!value.isBool()){ Log::error("ParticleSystem.%s.enabled: true or false", self.module.c_str()); return luabridge::LuaRef(L); }
            if (!m->hasEnable){ Log::error("ParticleSystem.%s is always on", self.module.c_str()); return luabridge::LuaRef(L); }
            m->setPresent(document, true);
            m->setEnabled(document, value.unsafe_cast<bool>());
            system.setEffectDocument(document);
            return luabridge::LuaRef(L);
        }
        const std::string suffix = "Multiplier";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0){
            const ParticleModules::Resolved r = ParticleModules::resolve(document, pathOf(self.module, name.substr(0, name.size() - suffix.size())));
            if (r.field && r.field->type == FieldType::MinMaxCurve){
                if (!value.isNumber()){ Log::error("ParticleSystem.%s.%s: a number", self.module.c_str(), name.c_str()); return luabridge::LuaRef(L); }
                MinMaxCurve& c = *(MinMaxCurve*)r.storage;
                const float v = value.unsafe_cast<float>() * ((r.field->flags & FieldFlag_Degrees) ? kRadToDeg : 1.0f);
                if (c.usesCurve()){
                    c.multiplier = v;
                }else{
                    // a constant's multiplier is the constant (Unity); a range scales both ends
                    if (c.mode == CurveMode::TwoConstants && c.constant != 0.0f){
                        c.constantMax *= v / c.constant;
                    }else{
                        c.constantMax = v;
                    }
                    c.constant = v;
                }
                if (r.module) r.module->setPresent(document, true);
                system.setEffectDocument(document);
                return luabridge::LuaRef(L);
            }
        }
        const ParticleModules::Resolved r = ParticleModules::resolve(document, pathOf(self.module, name));
        if (!r.field || !r.member.empty()){
            Log::error("ParticleSystem.%s has no field '%s' (docs/mcp/guide/particles.md lists them)", self.module.c_str(), name.c_str());
            return luabridge::LuaRef(L);
        }
        std::string why;
        if (!fromLua(*r.field, value, r.storage, why)){
            Log::error("ParticleSystem.%s.%s: %s", self.module.c_str(), name.c_str(), why.c_str());
            return luabridge::LuaRef(L);
        }
        // a module set from a script shows (Unity's modules are always there)
        if (r.module) r.module->setPresent(document, true);
        system.setEffectDocument(document);
        return luabridge::LuaRef(L);
    }

    ParticleSystemModule moduleOf(const ParticleSystem* constPs, const char* name){
        ParticleSystem* ps = const_cast<ParticleSystem*>(constPs);
        ParticleSystemModule m;
        m.scene = ps->getScene();
        m.entity = ps->getEntity();
        m.module = moduleKey(name);
        return m;
    }

    // a namespace of an enum's values as their effect names (Unity's names as the keys)
    void enumNamespace(lua_State* L, const char* name, std::initializer_list<std::pair<const char*, const char*>> values){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (const auto& [unity, json] : values) table[unity] = std::string(json);
        luabridge::setGlobal(L, table, name);
    }

}

void LuaBinding::registerParticleClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleSystemModule>("ParticleSystemModule")
        // Unity's GetBurst(index) / SetBurst(index, burst) / SetBursts(bursts): 0-based; index ==
        // burstCount appends
        .addFunction("getBurst", +[](ParticleSystemModule* self, int index, lua_State* L) -> luabridge::LuaRef {
            if (!alive(*self)) return luabridge::LuaRef(L);
            const ParticleEffectDocument document = systemOf(*self).getEffectDocument();
            if (index < 0 || (size_t)index >= document.emission.bursts.size()){
                Log::error("ParticleSystem.emission:getBurst(%d): the index is 0..burstCount-1 (burstCount %d)", index, (int)document.emission.bursts.size());
                return luabridge::LuaRef(L);
            }
            return burstToTable(L, document.emission.bursts[(size_t)index]);
        })
        .addFunction("setBurst", +[](ParticleSystemModule* self, int index, const luabridge::LuaRef& burst) -> bool {
            if (!alive(*self)) return false;
            ParticleSystem system = systemOf(*self);
            ParticleEffectDocument document = system.getEffectDocument();
            std::vector<Burst>& bursts = document.emission.bursts;
            if (index < 0 || (size_t)index > bursts.size() || (size_t)index >= kMaxBursts){
                Log::error("ParticleSystem.emission:setBurst(%d): the index is 0..burstCount (burstCount %d, at most %d)", index, (int)bursts.size(), (int)kMaxBursts);
                return false;
            }
            Burst value = (size_t)index < bursts.size() ? bursts[(size_t)index] : Burst();
            std::string why;
            if (!tableToBurst(burst, value, why)){
                Log::error("ParticleSystem.emission:setBurst: %s", why.c_str());
                return false;
            }
            if ((size_t)index == bursts.size()) bursts.push_back(value);
            else bursts[(size_t)index] = value;
            document.emission.present = true;
            system.setEffectDocument(document);
            return true;
        })
        .addFunction("setBursts", +[](ParticleSystemModule* self, const luabridge::LuaRef& list) -> bool {
            if (!alive(*self)) return false;
            if (!list.isTable() || (size_t)list.length() > kMaxBursts){
                Log::error("ParticleSystem.emission:setBursts: a table of at most %d bursts", (int)kMaxBursts);
                return false;
            }
            ParticleSystem system = systemOf(*self);
            ParticleEffectDocument document = system.getEffectDocument();
            std::vector<Burst> bursts;
            for (int i = 1; i <= list.length(); i++){
                Burst b;
                std::string why;
                if (!tableToBurst(list[i], b, why)){
                    Log::error("ParticleSystem.emission:setBursts: burst %d: %s", i, why.c_str());
                    return false;
                }
                bursts.push_back(b);
            }
            document.emission.bursts = std::move(bursts);
            document.emission.present = true;
            system.setEffectDocument(document);
            return true;
        })
        .addIndexMetaMethod(&moduleIndex)
        .addNewIndexMetaMethod(&moduleNewIndex)
        .endClass();

    // a module's colour field, live (S49 R40): reads as a MinMaxGradient, :setRamp writes it back
    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleSystemGradient>("ParticleSystemGradient")
        .addFunction("setRamp", +[](ParticleSystemGradient* self, const std::string& palette, const std::string& ramp, std::optional<int> steps) -> bool {
            MinMaxGradient g;
            if (!gradientOf(*self, g)) return false;
            const int n = steps.value_or(0);
            if (n != 0 && (n < 2 || n > kMaxRampColors)){
                Log::error("ParticleSystemGradient:setRamp: steps is 0 (the ramp as it is) or 2 to 32");
                return false;
            }
            applyRamp(g, palette, ramp, n);
            return setGradientOf(*self, g);
        })
        .addFunction("evaluate", +[](ParticleSystemGradient* self, float time, std::optional<float> random) -> Vector4 {
            MinMaxGradient g;
            return gradientOf(*self, g) ? g.evaluate(time, random.value_or(0.0f)) : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        })
        // a copy, detached from the system
        .addFunction("value", +[](ParticleSystemGradient* self) -> MinMaxGradient {
            MinMaxGradient g;
            gradientOf(*self, g);
            return g;
        })
        .addIndexMetaMethod(&gradientIndex)
        .addNewIndexMetaMethod(&gradientNewIndex)
        .endClass();

    // the module proxies on ParticleSystem (Unity's names), and Unity's control with children
    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleSystem>("ParticleSystem")
        .addProperty("main", +[](const ParticleSystem* self) { return moduleOf(self, "main"); })
        .addProperty("emission", +[](const ParticleSystem* self) { return moduleOf(self, "emission"); })
        .addProperty("shape", +[](const ParticleSystem* self) { return moduleOf(self, "shape"); })
        .addProperty("velocityOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "velocityOverLifetime"); })
        .addProperty("limitVelocityOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "limitVelocityOverLifetime"); })
        .addProperty("inheritVelocity", +[](const ParticleSystem* self) { return moduleOf(self, "inheritVelocity"); })
        .addProperty("forceOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "forceOverLifetime"); })
        .addProperty("noise", +[](const ParticleSystem* self) { return moduleOf(self, "noise"); })
        .addProperty("colorOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "colorOverLifetime"); })
        .addProperty("colorBySpeed", +[](const ParticleSystem* self) { return moduleOf(self, "colorBySpeed"); })
        .addProperty("sizeOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "sizeOverLifetime"); })
        .addProperty("sizeBySpeed", +[](const ParticleSystem* self) { return moduleOf(self, "sizeBySpeed"); })
        .addProperty("rotationOverLifetime", +[](const ParticleSystem* self) { return moduleOf(self, "rotationOverLifetime"); })
        .addProperty("rotationBySpeed", +[](const ParticleSystem* self) { return moduleOf(self, "rotationBySpeed"); })
        .addProperty("textureSheetAnimation", +[](const ParticleSystem* self) { return moduleOf(self, "textureSheetAnimation"); })
        .addProperty("collision", +[](const ParticleSystem* self) { return moduleOf(self, "collision"); })
        .addProperty("trails", +[](const ParticleSystem* self) { return moduleOf(self, "trails"); })
        .addProperty("customData", +[](const ParticleSystem* self) { return moduleOf(self, "customData"); })
        .addProperty("externalForces", +[](const ParticleSystem* self) { return moduleOf(self, "externalForces"); })
        .addProperty("renderer", +[](const ParticleSystem* self) { return moduleOf(self, "renderer"); })
        // pixel particles (S49): the Pixel Art module, the simulation mode, the Analytic twin
        .addProperty("pixelArt", +[](const ParticleSystem* self) { return moduleOf(self, "pixelArt"); })
        .addProperty("simulationMode",
            +[](const ParticleSystem* self) -> std::string {
                return self->getEffectDocument().simulationMode == SimulationMode::Analytic ? "analytic" : "simulated";
            },
            +[](ParticleSystem* self, const std::string& mode) {
                if (mode != "simulated" && mode != "analytic"){
                    Log::error("ParticleSystem.simulationMode: ParticleSimulationMode.Simulated or .Analytic");
                    return;
                }
                ParticleEffectDocument document = self->getEffectDocument();
                document.simulationMode = mode == "analytic" ? SimulationMode::Analytic : SimulationMode::Simulated;
                self->setEffectDocument(document);
                // the other mode starts over (R40)
                self->restart();
            })
        // the CPU twin of an Analytic particle (slot, emitter time); nil for a Simulated system
        .addFunction("getAnalyticParticle", +[](ParticleSystem* self, int slot, double time, lua_State* L) -> luabridge::LuaRef {
            const ParticleSystemComponent* particles = self->getScene()->findComponent<ParticleSystemComponent>(self->getEntity());
            const ParticleSystemState* state = particles ? particles->state.get() : nullptr;
            if (!state || !state->runtime.isAnalytic() || slot < 0)
                return luabridge::LuaRef(L);
            const AnalyticParticle p = AnalyticParticles::evaluateAt(*state->runtime.analytic(), (uint32_t)slot, time);
            luabridge::LuaRef t = luabridge::newTable(L);
            t["alive"] = p.alive;
            t["index"] = (double)p.index;
            t["age"] = p.age;
            t["life"] = p.life;
            t["position"] = p.position;
            t["velocity"] = p.velocity;
            t["rotation"] = p.rotation;
            t["size"] = p.size;
            t["color"] = Vector4((float)(p.color & 255u) / 255.0f, (float)((p.color >> 8) & 255u) / 255.0f,
                                 (float)((p.color >> 16) & 255u) / 255.0f, (float)(p.color >> 24) / 255.0f);
            t["rampStep"] = p.rampStep == 255 ? -1 : (int)p.rampStep;
            return t;
        })
        // Unity's Clear(withChildren = true)
        .addFunction("clear", +[](ParticleSystem* self, lua_State* L) {
            self->clear(lua_gettop(L) < 2 || lua_isnil(L, 2) || lua_toboolean(L, 2) != 0);
        })
        .endClass();

    // Unity's ParticleSystemStopBehavior (ps:stop(withChildren, behavior))
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ParticleSystemStopBehavior")
        .addVariable("StopEmittingAndClear", 0)
        .addVariable("StopEmitting", 1)
        .endNamespace();

    // Unity's enums of the module fields, as the names the fields hold
    enumNamespace(L, "ParticleSystemSimulationSpace", {{"Local", "local"}, {"World", "world"}});
    enumNamespace(L, "ParticleSystemScalingMode", {{"Hierarchy", "hierarchy"}, {"Local", "local"}, {"Shape", "shape"}});
    enumNamespace(L, "ParticleSystemStopAction", {{"None", "none"}, {"Disable", "disable"}, {"Destroy", "destroy"}, {"Callback", "callback"}});
    enumNamespace(L, "ParticleSystemEmitterVelocityMode", {{"Transform", "transform"}, {"Rigidbody", "rigidbody"}});
    enumNamespace(L, "ParticleSystemGravitySource", {{"Physics3D", "physics3D"}, {"Physics2D", "physics2D"}});
    enumNamespace(L, "ParticleSystemInheritVelocityMode", {{"Initial", "initial"}, {"Current", "current"}});
    enumNamespace(L, "ParticleSystemShapeType", {{"Sphere", "sphere"}, {"Hemisphere", "hemisphere"}, {"Cone", "cone"}, {"Box", "box"},
                                                 {"Circle", "circle"}, {"Donut", "donut"}, {"Edge", "edge"}, {"Rectangle", "rectangle"}, {"Mesh", "mesh"}});
    enumNamespace(L, "ParticleSystemShapeMultiModeValue", {{"Random", "random"}, {"Loop", "loop"}, {"PingPong", "pingPong"}, {"BurstSpread", "burstSpread"}});
    enumNamespace(L, "ParticleSystemAnimationMode", {{"Grid", "grid"}, {"Sprites", "sprites"}});
    enumNamespace(L, "ParticleSystemAnimationType", {{"WholeSheet", "wholeSheet"}, {"SingleRow", "singleRow"}});
    enumNamespace(L, "ParticleSystemAnimationRowMode", {{"Custom", "custom"}, {"Random", "random"}});
    enumNamespace(L, "ParticleSystemAnimationTimeMode", {{"Lifetime", "lifetime"}, {"FPS", "fps"}, {"Speed", "speed"}});
    enumNamespace(L, "ParticleSystemRenderMode", {{"Billboard", "billboard"}, {"Stretch", "stretchedBillboard"},
                                                  {"HorizontalBillboard", "horizontalBillboard"}, {"VerticalBillboard", "verticalBillboard"},
                                                  {"Mesh", "mesh"}, {"None", "none"}, {"FacingCameraPosition", "facingCameraPosition"},
                                                  {"WorldOriented", "worldOriented"}, {"LocalOriented", "localOriented"},
                                                  {"VelocityAligned", "velocityAligned"}, {"Pixel", "pixel"}});
    // pixel particles (S49)
    enumNamespace(L, "ParticlePixelShape", {{"Dot", "dot"}, {"Cross", "cross"}, {"Disc", "disc"}, {"Ring", "ring"}, {"Streak", "streak"},
                                            {"Puff", "puff"}, {"Shard", "shard"}, {"Chunk", "chunk"}, {"Glow", "glow"}});
    enumNamespace(L, "ParticleFade", {{"Dither", "dither"}, {"Cutout", "cutout"}, {"Alpha", "alpha"}});
    enumNamespace(L, "ParticleSimulationMode", {{"Simulated", "simulated"}, {"Analytic", "analytic"}});
    enumNamespace(L, "ParticleSystemSortMode", {{"None", "none"}, {"Distance", "byDistance"}, {"OldestInFront", "oldestInFront"},
                                                {"YoungestInFront", "youngestInFront"}, {"Depth", "byDepth"}});
    enumNamespace(L, "ParticleSystemRenderSpace", {{"View", "view"}, {"World", "world"}, {"Local", "local"}, {"Facing", "facing"}, {"Velocity", "velocity"}});
#endif
}
