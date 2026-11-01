// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Particle Systems of version 2 in Lua (particles spec 6.6): Unity's MinMaxCurve and
// MinMaxGradient, the ParticleSystem* enums and the module proxies (ps.main, ps.emission, ...:
// ParticleSystemModule, whose fields are particles/ParticleFields' by their Unity names).
// The ParticleSystem class itself is in ActionClassesLua.cpp.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "Scene.h"
#include "action/ParticleSystem.h"
#include "action/ParticleSystemModule.h"
#include "particles/MinMax.h"
#include "particles/ParticleFields.h"
#include "subsystem/ActionSystem.h"
#include "util/Color.h"

#include <cmath>

using namespace jasperengine;

namespace luabridge
{
    template<> struct Stack<ParticleSystemCurveMode> : EnumWrapper<ParticleSystemCurveMode>{};
    template<> struct Stack<ParticleSystemGradientMode> : EnumWrapper<ParticleSystemGradientMode>{};
}

namespace {

    // Unity's names a field also answers to (startSizeX is startSize, ...)
    std::string aliasOf(const std::string& module, const std::string& name){
        if (module == "main"){
            if (name == "startSizeX") return "startSize";
            if (name == "startRotationZ") return "startRotation";
        }
        if (module == "noise" && name == "strengthX") return "strength";
        if ((module == "sizeOverLifetime" || module == "sizeBySpeed") && name == "x") return "size";
        return name;
    }

    // the field a key names, and whether it is the multiplier of a MinMaxCurve ("xMultiplier")
    const particles::FieldInfo* resolveField(const std::string& module, std::string name, bool& multiplier){
        multiplier = false;
        const std::string suffix = "Multiplier";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0){
            const std::string base = aliasOf(module, name.substr(0, name.size() - suffix.size()));
            const particles::FieldInfo* field = particles::findField(module + "." + base);
            if (field && field->type == particles::FieldType::Curve){
                multiplier = true;
                return field;
            }
        }
        return particles::findField(module + "." + aliasOf(module, name));
    }

    luabridge::LuaRef curveRef(lua_State* L, const MinMaxCurve& value){
        return luabridge::LuaRef(L, value);
    }

    // a MinMaxCurve from a number, a MinMaxCurve or an AnimationCurve (a curve, multiplier 1)
    bool toCurve(const luabridge::LuaRef& value, MinMaxCurve& out){
        if (value.isNumber()){
            out = MinMaxCurve(value.unsafe_cast<float>());
            return std::isfinite(out.constant);
        }
        if (value.isInstance<MinMaxCurve>()){
            out = value.unsafe_cast<MinMaxCurve>();
            return true;
        }
        if (value.isInstance<AnimationCurve>()){
            out = MinMaxCurve::fromCurve(value.unsafe_cast<AnimationCurve>(), 1.0f);
            return true;
        }
        return false;
    }

    // a MinMaxGradient from a colour (Vector4 / Vector3, sRGB as the pickers), a Gradient or a
    // MinMaxGradient
    bool toGradient(const luabridge::LuaRef& value, MinMaxGradient& out){
        if (value.isInstance<MinMaxGradient>()){
            out = value.unsafe_cast<MinMaxGradient>();
            return true;
        }
        if (value.isInstance<Vector4>()){
            out = MinMaxGradient::fromColor(Color::sRGBToLinear(value.unsafe_cast<Vector4>()));
            return true;
        }
        if (value.isInstance<Vector3>()){
            const Vector3 c = Color::sRGBToLinear(value.unsafe_cast<Vector3>());
            out = MinMaxGradient::fromColor(Vector4(c.x, c.y, c.z, 1.0f));
            return true;
        }
        if (value.isInstance<Gradient>()){
            out = MinMaxGradient::fromGradient(value.unsafe_cast<Gradient>());
            return true;
        }
        return false;
    }

    luabridge::LuaRef burstToTable(lua_State* L, const ParticleEmissionBurst& burst){
        luabridge::LuaRef table = luabridge::newTable(L);
        table["time"] = burst.time;
        table["count"] = burst.count;
        table["cycles"] = burst.cycles;
        table["interval"] = burst.interval;
        table["probability"] = burst.probability;
        return table;
    }

    // a burst table {time, count (number or MinMaxCurve), cycles, interval, probability}
    bool tableToBurst(const luabridge::LuaRef& table, ParticleEmissionBurst& out, std::string& error){
        if (!table.isTable()){
            error = "a burst is a table {time=, count=, cycles=, interval=, probability=}";
            return false;
        }
        const luabridge::LuaRef time = table["time"];
        const luabridge::LuaRef count = table["count"];
        const luabridge::LuaRef cycles = table["cycles"];
        const luabridge::LuaRef interval = table["interval"];
        const luabridge::LuaRef probability = table["probability"];
        if (!time.isNil()){
            if (!time.isNumber()){ error = "burst.time must be a number"; return false; }
            out.time = std::max(0.0f, time.unsafe_cast<float>());
        }
        if (!count.isNil() && !toCurve(count, out.count)){
            error = "burst.count must be a number or a MinMaxCurve";
            return false;
        }
        if (!cycles.isNil()){
            if (!cycles.isNumber()){ error = "burst.cycles must be an integer (0 = forever)"; return false; }
            out.cycles = std::max(0, cycles.unsafe_cast<int>());
        }
        if (!interval.isNil()){
            if (!interval.isNumber()){ error = "burst.interval must be a number"; return false; }
            out.interval = std::max(0.0001f, interval.unsafe_cast<float>());
        }
        if (!probability.isNil()){
            if (!probability.isNumber()){ error = "burst.probability must be a number"; return false; }
            out.probability = std::min(1.0f, std::max(0.0f, probability.unsafe_cast<float>()));
        }
        return true;
    }

    luabridge::LuaRef fieldToLua(lua_State* L, const particles::FieldInfo& field, ParticleSystemComponent& c, bool multiplier){
        void* value = field.get(c);
        if (multiplier){
            return luabridge::LuaRef(L, static_cast<MinMaxCurve*>(value)->constant);
        }
        switch (field.type){
            case particles::FieldType::Bool: return luabridge::LuaRef(L, *static_cast<bool*>(value));
            case particles::FieldType::Int:
            case particles::FieldType::Enum: return luabridge::LuaRef(L, *static_cast<int*>(value));
            case particles::FieldType::UInt: return luabridge::LuaRef(L, *static_cast<unsigned int*>(value));
            case particles::FieldType::Float: return luabridge::LuaRef(L, *static_cast<float*>(value));
            case particles::FieldType::Vector2: return luabridge::LuaRef(L, *static_cast<Vector2*>(value));
            case particles::FieldType::Vector3: return luabridge::LuaRef(L, *static_cast<Vector3*>(value));
            case particles::FieldType::Curve: return curveRef(L, *static_cast<MinMaxCurve*>(value));
            case particles::FieldType::Gradient: return luabridge::LuaRef(L, *static_cast<MinMaxGradient*>(value));
            case particles::FieldType::String: return luabridge::LuaRef(L, *static_cast<std::string*>(value));
            case particles::FieldType::IntList: {
                luabridge::LuaRef table = luabridge::newTable(L);
                int i = 1;
                for (int v : *static_cast<std::vector<int>*>(value)) table[i++] = v;
                return table;
            }
            case particles::FieldType::Bursts: {
                luabridge::LuaRef table = luabridge::newTable(L);
                int i = 1;
                for (const ParticleEmissionBurst& burst : *static_cast<std::vector<ParticleEmissionBurst>*>(value)) table[i++] = burstToTable(L, burst);
                return table;
            }
        }
        return luabridge::LuaRef(L);
    }

    // writes a field; false with `error` for a value of the wrong kind
    bool luaToField(const particles::FieldInfo& field, ParticleSystemComponent& c, const luabridge::LuaRef& value, bool multiplier, std::string& error){
        void* target = field.get(c);
        if (multiplier){
            if (!value.isNumber()){ error = "a number"; return false; }
            static_cast<MinMaxCurve*>(target)->constant = value.unsafe_cast<float>();
            return true;
        }
        switch (field.type){
            case particles::FieldType::Bool:
                if (!value.isBool()){ error = "a boolean"; return false; }
                *static_cast<bool*>(target) = value.unsafe_cast<bool>();
                return true;
            case particles::FieldType::Int:
                if (!value.isNumber()){ error = "an integer"; return false; }
                *static_cast<int*>(target) = value.unsafe_cast<int>();
                return true;
            case particles::FieldType::UInt:
                if (!value.isNumber() || value.unsafe_cast<double>() < 0.0){ error = "an integer >= 0"; return false; }
                *static_cast<unsigned int*>(target) = value.unsafe_cast<unsigned int>();
                return true;
            case particles::FieldType::Float:
                if (!value.isNumber()){ error = "a number"; return false; }
                *static_cast<float*>(target) = value.unsafe_cast<float>();
                return true;
            case particles::FieldType::Vector2:
                if (!value.isInstance<Vector2>()){ error = "a Vector2"; return false; }
                *static_cast<Vector2*>(target) = value.unsafe_cast<Vector2>();
                return true;
            case particles::FieldType::Vector3:
                if (!value.isInstance<Vector3>()){ error = "a Vector3"; return false; }
                *static_cast<Vector3*>(target) = value.unsafe_cast<Vector3>();
                return true;
            case particles::FieldType::Curve: {
                MinMaxCurve curve;
                if (!toCurve(value, curve)){ error = "a number, a MinMaxCurve or an AnimationCurve"; return false; }
                *static_cast<MinMaxCurve*>(target) = curve;
                return true;
            }
            case particles::FieldType::Gradient: {
                MinMaxGradient gradient;
                if (!toGradient(value, gradient)){ error = "a colour (Vector4 / Vector3, sRGB), a Gradient or a MinMaxGradient"; return false; }
                *static_cast<MinMaxGradient*>(target) = gradient;
                return true;
            }
            case particles::FieldType::Enum: {
                int v = -1;
                if (value.isNumber()) v = value.unsafe_cast<int>();
                else if (value.isString()) v = particles::enumValue(field, value.unsafe_cast<std::string>());
                if (!field.enumNames || v < 0 || (size_t)v >= field.enumNames->size()){
                    error = "one of its enum's values";
                    for (size_t i = 0; field.enumNames && i < field.enumNames->size(); i++) error += (i ? ", " : " (") + (*field.enumNames)[i];
                    error += ")";
                    return false;
                }
                *static_cast<int*>(target) = v;
                return true;
            }
            case particles::FieldType::String:
                if (!value.isString()){ error = "a string"; return false; }
                *static_cast<std::string*>(target) = value.unsafe_cast<std::string>();
                return true;
            case particles::FieldType::IntList: {
                if (!value.isTable()){ error = "a table of integers"; return false; }
                std::vector<int> list;
                for (int i = 1; i <= (int)value.length(); i++){
                    const luabridge::LuaRef item = value[i];
                    if (!item.isNumber()){ error = "a table of integers"; return false; }
                    list.push_back(item.unsafe_cast<int>());
                }
                *static_cast<std::vector<int>*>(target) = list;
                return true;
            }
            case particles::FieldType::Bursts: {
                if (!value.isTable()){ error = "a table of bursts"; return false; }
                std::vector<ParticleEmissionBurst> bursts;
                for (int i = 1; i <= (int)value.length(); i++){
                    ParticleEmissionBurst burst;
                    if (!tableToBurst(value[i], burst, error)) return false;
                    bursts.push_back(burst);
                }
                *static_cast<std::vector<ParticleEmissionBurst>*>(target) = bursts;
                c.state.bursts.resize(bursts.size());
                return true;
            }
        }
        return false;
    }

    ParticleSystemComponent& moduleComponent(const ParticleSystemModule& self, lua_State* L){
        ParticleSystemComponent* c = self.component();
        if (!c){
            luaL_error(L, "ParticleSystem.%s: the GameObject or its Particle System is gone", self.moduleName());
        }
        return *c;
    }

    // writes from Lua need the modules (spec 7: nothing silently does nothing on version 1)
    void requireModules(const ParticleSystemModule& self, ParticleSystemComponent& c, lua_State* L, const std::string& name){
        if (c.version < 2){
            luaL_error(L, "ParticleSystem.%s.%s: this Particle System is version 1 (the legacy model): call ps:upgrade() "
                          "(or set ps.version = 2) to use the modules", self.moduleName(), name.c_str());
        }
    }

    bool isModuleMethod(const std::string& name){
        return name == "setBursts" || name == "getBurst" || name == "setBurst";
    }

    luabridge::LuaRef moduleIndex(ParticleSystemModule& self, const luabridge::LuaRef& key, lua_State* L){
        if (!key.isString()) return luabridge::LuaRef(L);
        const std::string name = key.unsafe_cast<std::string>();
        // nil for the methods: LuaBridge then finds them
        if (isModuleMethod(name) || name.rfind("__", 0) == 0) return luabridge::LuaRef(L);
        ParticleSystemComponent& c = moduleComponent(self, L);
        const std::string module = self.moduleName();
        if (module == "emission" && name == "burstCount") return luabridge::LuaRef(L, (int)c.emission.bursts.size());
        // Unity's main.useUnscaledTime: the component's own field (shared with ui-scaling-text)
        if (module == "main" && name == "useUnscaledTime") return luabridge::LuaRef(L, c.useUnscaledTime);
        bool multiplier = false;
        const particles::FieldInfo* field = resolveField(module, name, multiplier);
        if (!field){
            luaL_error(L, "ParticleSystem.%s has no field '%s' (docs/mcp/guide/particles.md lists them)", module.c_str(), name.c_str());
        }
        return fieldToLua(L, *field, c, multiplier);
    }

    luabridge::LuaRef moduleNewIndex(ParticleSystemModule& self, const luabridge::LuaRef& key, const luabridge::LuaRef& value, lua_State* L){
        if (!key.isString()) return luabridge::LuaRef(L);
        const std::string name = key.unsafe_cast<std::string>();
        ParticleSystemComponent& c = moduleComponent(self, L);
        const std::string module = self.moduleName();
        if (module == "main" && name == "useUnscaledTime"){
            if (!value.isBool()) luaL_error(L, "ParticleSystem.main.useUnscaledTime: expected a boolean");
            c.useUnscaledTime = value.unsafe_cast<bool>();
            return luabridge::LuaRef(L);
        }
        bool multiplier = false;
        const particles::FieldInfo* field = resolveField(module, name, multiplier);
        if (!field){
            luaL_error(L, "ParticleSystem.%s has no field '%s' (docs/mcp/guide/particles.md lists them)", module.c_str(), name.c_str());
        }
        requireModules(self, c, L, name);
        std::string error;
        if (!luaToField(*field, c, value, multiplier, error)){
            luaL_error(L, "ParticleSystem.%s.%s: expected %s", module.c_str(), name.c_str(), error.c_str());
        }
        // the renderer's mesh: the target draws it now (a primitive; models are the editor's)
        if (module == "renderer" && name == "mesh"){
            const Entity target = self.scene->getSystem<ActionSystem>()->particleTarget(self.entity);
            if (!ParticleSystem::applyRendererMesh(self.scene, target, c.renderer.mesh)){
                Log::warn("ParticleSystem.renderer.mesh = '%s': only the primitives (quad, cube, sphere, cylinder, capsule, plane) "
                          "change at runtime; a model is set in the editor", c.renderer.mesh.c_str());
            }
        }
        return luabridge::LuaRef(L);
    }

    // emission:setBursts({ {time=0, count=10, cycles=1, interval=0.01, probability=1}, ... })
    int moduleSetBursts(lua_State* L){
        auto self = luabridge::get<ParticleSystemModule*>(L, 1);
        if (!self || !*self || std::string((*self)->moduleName()) != "emission"){
            return luaL_error(L, "setBursts: call it on ps.emission (ps.emission:setBursts({...}))");
        }
        ParticleSystemComponent& c = moduleComponent(**self, L);
        requireModules(**self, c, L, "setBursts");
        const luabridge::LuaRef list = luabridge::LuaRef::fromStack(L, 2);
        std::string error;
        if (!luaToField(*particles::findField("emission.bursts"), c, list, false, error)){
            return luaL_error(L, "ParticleSystem.emission:setBursts: %s", error.c_str());
        }
        return 0;
    }

}

ParticleSystemComponent* ParticleSystemModule::component() const{
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return nullptr;
    return scene->findComponent<ParticleSystemComponent>(entity);
}

const char* ParticleSystemModule::moduleName() const{
    const auto& names = particles::moduleNames();
    if (module < 0 || (size_t)module >= names.size()) return "main";
    return names[(size_t)module].c_str();
}

void LuaBinding::registerParticleClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // ---- enums (Unity's)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ParticleSystemCurveMode")
        .addVariable("Constant", ParticleSystemCurveMode::Constant)
        .addVariable("Curve", ParticleSystemCurveMode::Curve)
        .addVariable("TwoCurves", ParticleSystemCurveMode::TwoCurves)
        .addVariable("TwoConstants", ParticleSystemCurveMode::TwoConstants)
        .endNamespace()
        .beginNamespace("ParticleSystemGradientMode")
        .addVariable("Color", ParticleSystemGradientMode::Color)
        .addVariable("Gradient", ParticleSystemGradientMode::Gradient)
        .addVariable("TwoColors", ParticleSystemGradientMode::TwoColors)
        .addVariable("TwoGradients", ParticleSystemGradientMode::TwoGradients)
        .addVariable("RandomColor", ParticleSystemGradientMode::RandomColor)
        .endNamespace()
        .beginNamespace("ParticleSystemShapeType")
        .addVariable("Sphere", ParticleSystemShapeType::Sphere)
        .addVariable("Hemisphere", ParticleSystemShapeType::Hemisphere)
        .addVariable("Cone", ParticleSystemShapeType::Cone)
        .addVariable("ConeVolume", ParticleSystemShapeType::ConeVolume)
        .addVariable("Box", ParticleSystemShapeType::Box)
        .addVariable("BoxShell", ParticleSystemShapeType::BoxShell)
        .addVariable("BoxEdge", ParticleSystemShapeType::BoxEdge)
        .addVariable("Circle", ParticleSystemShapeType::Circle)
        .addVariable("Edge", ParticleSystemShapeType::Edge)
        .addVariable("Rectangle", ParticleSystemShapeType::Rectangle)
        .addVariable("Donut", ParticleSystemShapeType::Donut)
        .endNamespace()
        .beginNamespace("ParticleSystemShapeMultiModeValue")
        .addVariable("Random", ParticleSystemShapeMultiModeValue::Random)
        .addVariable("Loop", ParticleSystemShapeMultiModeValue::Loop)
        .addVariable("PingPong", ParticleSystemShapeMultiModeValue::PingPong)
        .addVariable("BurstSpread", ParticleSystemShapeMultiModeValue::BurstSpread)
        .endNamespace()
        .beginNamespace("ParticleSystemSimulationSpace")
        .addVariable("Local", ParticleSystemSimulationSpace::Local)
        .addVariable("World", ParticleSystemSimulationSpace::World)
        .endNamespace()
        .beginNamespace("ParticleSystemScalingMode")
        .addVariable("Hierarchy", ParticleSystemScalingMode::Hierarchy)
        .addVariable("Local", ParticleSystemScalingMode::Local)
        .addVariable("Shape", ParticleSystemScalingMode::Shape)
        .endNamespace()
        .beginNamespace("ParticleSystemStopAction")
        .addVariable("None", ParticleSystemStopAction::None)
        .addVariable("Disable", ParticleSystemStopAction::Disable)
        .addVariable("Destroy", ParticleSystemStopAction::Destroy)
        .addVariable("Callback", ParticleSystemStopAction::Callback)
        .endNamespace()
        .beginNamespace("ParticleSystemStopBehavior")
        .addVariable("StopEmittingAndClear", ParticleSystemStopBehavior::StopEmittingAndClear)
        .addVariable("StopEmitting", ParticleSystemStopBehavior::StopEmitting)
        .endNamespace()
        .beginNamespace("ParticleSystemEmitterVelocityMode")
        .addVariable("Transform", ParticleSystemEmitterVelocityMode::Transform)
        .addVariable("Rigidbody", ParticleSystemEmitterVelocityMode::Rigidbody)
        .endNamespace()
        .beginNamespace("ParticleSystemGravitySource")
        .addVariable("Physics3D", ParticleSystemGravitySource::Physics3D)
        .addVariable("Physics2D", ParticleSystemGravitySource::Physics2D)
        .endNamespace()
        .beginNamespace("ParticleSystemInheritVelocityMode")
        .addVariable("Initial", ParticleSystemInheritVelocityMode::Initial)
        .addVariable("Current", ParticleSystemInheritVelocityMode::Current)
        .endNamespace()
        .beginNamespace("ParticleSystemAnimationMode")
        .addVariable("Grid", ParticleSystemAnimationMode::Grid)
        .addVariable("Sprites", ParticleSystemAnimationMode::Sprites)
        .endNamespace()
        .beginNamespace("ParticleSystemAnimationType")
        .addVariable("WholeSheet", ParticleSystemAnimationType::WholeSheet)
        .addVariable("SingleRow", ParticleSystemAnimationType::SingleRow)
        .endNamespace()
        .beginNamespace("ParticleSystemAnimationRowMode")
        .addVariable("Custom", ParticleSystemAnimationRowMode::Custom)
        .addVariable("Random", ParticleSystemAnimationRowMode::Random)
        .endNamespace()
        .beginNamespace("ParticleSystemAnimationTimeMode")
        .addVariable("Lifetime", ParticleSystemAnimationTimeMode::Lifetime)
        .addVariable("Speed", ParticleSystemAnimationTimeMode::Speed)
        .addVariable("FPS", ParticleSystemAnimationTimeMode::FPS)
        .endNamespace()
        .beginNamespace("ParticleSystemRenderMode")
        .addVariable("Billboard", ParticleSystemRenderMode::Billboard)
        .addVariable("Stretch", ParticleSystemRenderMode::Stretch)
        .addVariable("HorizontalBillboard", ParticleSystemRenderMode::HorizontalBillboard)
        .addVariable("VerticalBillboard", ParticleSystemRenderMode::VerticalBillboard)
        .addVariable("Mesh", ParticleSystemRenderMode::Mesh)
        .endNamespace()
        .beginNamespace("ParticleSystemSortMode")
        .addVariable("None", ParticleSystemSortMode::None)
        .addVariable("Distance", ParticleSystemSortMode::Distance)
        .addVariable("OldestInFront", ParticleSystemSortMode::OldestInFront)
        .addVariable("YoungestInFront", ParticleSystemSortMode::YoungestInFront)
        .endNamespace()
        .beginNamespace("ParticleSystemRenderSpace")
        .addVariable("View", ParticleSystemRenderSpace::View)
        .addVariable("World", ParticleSystemRenderSpace::World)
        .addVariable("Local", ParticleSystemRenderSpace::Local)
        .addVariable("Facing", ParticleSystemRenderSpace::Facing)
        .addVariable("Velocity", ParticleSystemRenderSpace::Velocity)
        .endNamespace();

    // ---- MinMaxCurve: a value that is a constant, a curve, or random between two of them
    luabridge::getGlobalNamespace(L)
        .beginClass<MinMaxCurve>("MinMaxCurve")
        .addConstructor<void(), void(float), void(float, float), void(float, const AnimationCurve&),
                        void(float, const AnimationCurve&, const AnimationCurve&)>()
        .addProperty("mode",
            +[](const MinMaxCurve* self) -> ParticleSystemCurveMode { return self->mode; },
            +[](MinMaxCurve* self, ParticleSystemCurveMode mode) { self->mode = mode; })
        .addProperty("constant", &MinMaxCurve::constant, &MinMaxCurve::constant)
        .addProperty("constantMin", &MinMaxCurve::constantMin, &MinMaxCurve::constantMin)
        .addProperty("constantMax", &MinMaxCurve::constant, &MinMaxCurve::constant)
        .addProperty("curve", &MinMaxCurve::curve, &MinMaxCurve::curve)
        .addProperty("curveMin", &MinMaxCurve::curveMin, &MinMaxCurve::curveMin)
        .addProperty("curveMax", &MinMaxCurve::curve, &MinMaxCurve::curve)
        .addProperty("curveMultiplier", &MinMaxCurve::constant, &MinMaxCurve::constant)
        .addFunction("evaluate",
            luabridge::overload<const MinMaxCurve*, float>(+[](const MinMaxCurve* self, float t) -> float { return self->evaluate(t, 1.0f); }),
            luabridge::overload<const MinMaxCurve*, float, float>(+[](const MinMaxCurve* self, float t, float random01) -> float { return self->evaluate(t, random01); }))
        .addFunction("__eq", +[](const MinMaxCurve* a, const MinMaxCurve* b) -> bool { return a && b && *a == *b; })
        .endClass();

    // ---- MinMaxGradient: colours given and read in sRGB (as the pickers), stored linear
    luabridge::getGlobalNamespace(L)
        .beginClass<MinMaxGradient>("MinMaxGradient")
        .addConstructor(
            +[](void* ptr) { return new (ptr) MinMaxGradient(); },
            +[](void* ptr, const Vector4& color) { return new (ptr) MinMaxGradient(MinMaxGradient::fromColor(Color::sRGBToLinear(color))); },
            +[](void* ptr, const Vector3& color) {
                const Vector3 c = Color::sRGBToLinear(color);
                return new (ptr) MinMaxGradient(MinMaxGradient::fromColor(Vector4(c.x, c.y, c.z, 1.0f)));
            },
            +[](void* ptr, const Vector4& min, const Vector4& max) {
                return new (ptr) MinMaxGradient(MinMaxGradient::between(Color::sRGBToLinear(min), Color::sRGBToLinear(max)));
            },
            +[](void* ptr, const Gradient& gradient) { return new (ptr) MinMaxGradient(MinMaxGradient::fromGradient(gradient)); },
            +[](void* ptr, const Gradient& min, const Gradient& max) { return new (ptr) MinMaxGradient(MinMaxGradient::between(min, max)); })
        .addProperty("mode",
            +[](const MinMaxGradient* self) -> ParticleSystemGradientMode { return self->mode; },
            +[](MinMaxGradient* self, ParticleSystemGradientMode mode) { self->mode = mode; })
        // Color: the colour; TwoColors: the maximum (sRGB)
        .addProperty("color",
            +[](const MinMaxGradient* self) -> Vector4 { return Color::linearTosRGB(self->color); },
            +[](MinMaxGradient* self, const Vector4& color) { self->color = Color::sRGBToLinear(color); })
        .addProperty("colorMin",
            +[](const MinMaxGradient* self) -> Vector4 { return Color::linearTosRGB(self->colorMin); },
            +[](MinMaxGradient* self, const Vector4& color) { self->colorMin = Color::sRGBToLinear(color); })
        .addProperty("colorMax",
            +[](const MinMaxGradient* self) -> Vector4 { return Color::linearTosRGB(self->color); },
            +[](MinMaxGradient* self, const Vector4& color) { self->color = Color::sRGBToLinear(color); })
        // Gradient / RandomColor: gradient; TwoGradients: gradientMin .. gradientMax (copies)
        .addProperty("gradient", &MinMaxGradient::gradient, &MinMaxGradient::gradient)
        .addProperty("gradientMin", &MinMaxGradient::gradientMin, &MinMaxGradient::gradientMin)
        .addProperty("gradientMax", &MinMaxGradient::gradient, &MinMaxGradient::gradient)
        // linear RGBA (as Gradient:evaluate)
        .addFunction("evaluate",
            luabridge::overload<const MinMaxGradient*, float>(+[](const MinMaxGradient* self, float t) -> Vector4 { return self->evaluate(t, 1.0f); }),
            luabridge::overload<const MinMaxGradient*, float, float>(+[](const MinMaxGradient* self, float t, float random01) -> Vector4 { return self->evaluate(t, random01); }))
        .addFunction("__eq", +[](const MinMaxGradient* a, const MinMaxGradient* b) -> bool { return a && b && *a == *b; })
        .endClass();

    // ---- the module proxies (ps.main, ps.emission, ...)
    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleSystemModule>("ParticleSystemModule")
        .addFunction("setBursts", &moduleSetBursts)
        // Unity's GetBurst(index): 0-based; a table {time, count, cycles, interval, probability}
        .addFunction("getBurst", +[](ParticleSystemModule* self, int index, lua_State* L) -> luabridge::LuaRef {
            ParticleSystemComponent& c = moduleComponent(*self, L);
            if (index < 0 || (size_t)index >= c.emission.bursts.size()){
                luaL_error(L, "ParticleSystem.emission:getBurst(%d): the index is 0..burstCount-1 (burstCount %d)", index, (int)c.emission.bursts.size());
            }
            return burstToTable(L, c.emission.bursts[(size_t)index]);
        })
        // Unity's SetBurst(index, burst): 0-based; index == burstCount appends
        .addFunction("setBurst", +[](ParticleSystemModule* self, int index, const luabridge::LuaRef& burst, lua_State* L) {
            ParticleSystemComponent& c = moduleComponent(*self, L);
            requireModules(*self, c, L, "setBurst");
            if (index < 0 || (size_t)index > c.emission.bursts.size()){
                luaL_error(L, "ParticleSystem.emission:setBurst(%d): the index is 0..burstCount (burstCount %d)", index, (int)c.emission.bursts.size());
            }
            ParticleEmissionBurst value = (size_t)index < c.emission.bursts.size() ? c.emission.bursts[(size_t)index] : ParticleEmissionBurst();
            std::string error;
            if (!tableToBurst(burst, value, error)){
                luaL_error(L, "ParticleSystem.emission:setBurst: %s", error.c_str());
            }
            if ((size_t)index == c.emission.bursts.size()) c.emission.bursts.push_back(value);
            else c.emission.bursts[(size_t)index] = value;
            c.state.bursts.resize(c.emission.bursts.size());
        })
        .addIndexMetaMethod(&moduleIndex)
        .addNewIndexMetaMethod(&moduleNewIndex)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
