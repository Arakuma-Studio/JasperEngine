// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include <cassert>

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Vector2.h"
#include "Vector3.h"
#include "Vector4.h"
#include "Matrix3.h"
#include "Matrix4.h"
#include "Quaternion.h"
#include "Rect.h"
#include "EdgeInsets.h"
#include "Ray.h"
#include "Sphere.h"
#include "Plane.h"
#include "AABB.h"
#include "OBB.h"
#include "Curve.h"
#include "Log.h"
#include "particles/ParticleValues.h"

#include <optional>
#include <vector>

using namespace jasperengine;

namespace luabridge
{
    template<> struct Stack<Plane::Side> : EnumWrapper<Plane::Side>{};
    template<> struct Stack<AABB::BoxType> : EnumWrapper<AABB::BoxType>{};
    template<> struct Stack<AABB::CornerEnum> : EnumWrapper<AABB::CornerEnum>{};
    template<> struct Stack<OBB::BoxType> : EnumWrapper<OBB::BoxType>{};
    template<> struct Stack<OBB::CornerEnum> : EnumWrapper<OBB::CornerEnum>{};
    
    template<> struct Stack<RayFilter> : EnumWrapper<RayFilter>{};

    template<> struct Stack<CurveWrapMode> : EnumWrapper<CurveWrapMode>{};
    template<> struct Stack<CurveTangentMode> : EnumWrapper<CurveTangentMode>{};
    template<> struct Stack<GradientMode> : EnumWrapper<GradientMode>{};
    template<> struct Stack<particles::CurveMode> : EnumWrapper<particles::CurveMode>{};
    template<> struct Stack<particles::GradientValueMode> : EnumWrapper<particles::GradientValueMode>{};
}

// constants are returned as copies: bound by pointer, LuaBridge hands out a writable reference to them
template <class T, const T& Value>
static T constantCopy(){
    return Value;
}

// Operators. Bind them with exact signatures (luabridge::overload or a lambda), never with a
// C-style cast: a cast to a signature the class does not declare still compiles and calls the
// function with arguments of the wrong type (generate_lua_stubs.py reports such casts).

// __eq: Lua calls it for any two userdata (`v == q`, `v2 == v3`); a value of another class is
// not equal (a typed argument would raise an error instead). Compares a copy: some classes
// (Rect) declare a non-const operator==.
template <class T>
static bool equalsLua(const T& value, const luabridge::LuaRef& other){
    if (!other.isInstance<T>())
        return false;
    const auto otherValue = other.cast<T>();
    T copy(value);
    return otherValue && copy == *otherValue;
}

// `number <op> value` (2 * v, 1 / v). Lua calls the metamethod of whichever operand has one
// with the operands in order, so the number comes first; LuaBridge's overloads all take the
// object first. addNumberFirst wraps a registered metamethod (in the class and the const class
// metatables): a number first operand goes to Op(number, value), anything else to the
// overloads. The stubs need nothing: LuaLS reads `---@operator mul(number)` both ways round.
template <class T>
static T numberTimes(float number, const T& value){
    return value * number;
}

template <class T>
static T numberDividedBy(float number, const T& value){
    return number / value;
}

static Vector3 numberDividedByVector3(float number, const Vector3& value){
    return Vector3(number / value.x, number / value.y, number / value.z);
}

template <class T, T (*Op)(float, const T&)>
static int numberFirstMetamethod(lua_State* L){
    if (lua_type(L, 1) == LUA_TNUMBER) {
        const auto number = luabridge::Stack<float>::get(L, 1);
        const auto value = luabridge::Stack<T>::get(L, 2);
        if (number && value) {
            const auto pushed = luabridge::Stack<T>::push(L, Op(*number, *value));
            if (!pushed)
                return luaL_error(L, "%s", pushed.message().c_str());
            return 1;
        }
    }
    // the class's own metamethod (upvalue 1), with the same arguments
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

template <class T, T (*Op)(float, const T&)>
static void addNumberFirst(lua_State* L, const char* metamethod){
    for (const void* key : {luabridge::detail::getClassRegistryKey<T>(), luabridge::detail::getConstRegistryKey<T>()}) {
        lua_rawgetp(L, LUA_REGISTRYINDEX, key);
        lua_getfield(L, -1, metamethod);
        assert(lua_isfunction(L, -1));
        lua_pushcclosure(L, &numberFirstMetamethod<T, Op>, 1);
        lua_setfield(L, -2, metamethod);
        lua_pop(L, 1);
    }
}

// ---- curves and gradients (docs/specs/S27-editor-tools.md R43): Unity's names, 1-based key
// indices (C++ and MCP are 0-based). Lua is compiled as C, so nothing here raises a Lua error:
// a bad index gives nil, false or -1.

namespace {

    bool keyIndex(const Curve& curve, int index){
        return index >= 1 && (size_t)index <= curve.keys.size();
    }

    // the 1-based index of a 0-based one, -1 kept
    int luaIndex(int index){
        return index < 0 ? -1 : index + 1;
    }

    // a key of a Gradient's colorKeys / alphaKeys argument: a GradientColorKey / GradientAlphaKey,
    // or (upstream's lines-and-trails form) a {time, Vector3 sRGB colour} / {time, alpha} table
    bool keyEntry(const luabridge::LuaRef& entry, GradientColorKey& key){
        if (entry.isInstance<GradientColorKey>()){
            const auto cast = entry.cast<GradientColorKey>();
            if (!cast) return false;
            key = *cast;
            return true;
        }
        if (!entry.isTable() || entry.length() < 2) return false;
        const luabridge::LuaRef time = entry[1];
        const luabridge::LuaRef color = entry[2];
        if (!time.isNumber() || !color.isInstance<Vector3>()) return false;
        const auto c = color.cast<Vector3>();
        if (!c) return false;
        key = GradientColorKey{time.unsafe_cast<float>(), *c};
        return true;
    }

    bool keyEntry(const luabridge::LuaRef& entry, GradientAlphaKey& key){
        if (entry.isInstance<GradientAlphaKey>()){
            const auto cast = entry.cast<GradientAlphaKey>();
            if (!cast) return false;
            key = *cast;
            return true;
        }
        if (!entry.isTable() || entry.length() < 2) return false;
        const luabridge::LuaRef time = entry[1];
        const luabridge::LuaRef alpha = entry[2];
        if (!time.isNumber() || !alpha.isNumber()) return false;
        key = GradientAlphaKey{time.unsafe_cast<float>(), alpha.unsafe_cast<float>()};
        return true;
    }

    // the keys of a colorKeys / alphaKeys argument, entries that are not keys skipped; false
    // when it holds more than Gradient::kMaxKeys keys (Unity's limit: nothing is set then)
    template <class T>
    bool keyList(const luabridge::LuaRef& table, std::vector<T>& keys){
        keys.clear();
        if (!table.isTable())
            return true;
        const int count = table.length();
        for (int i = 1; i <= count; i++){
            T key;
            if (keyEntry(table[i], key))
                keys.push_back(key);
        }
        return keys.size() <= Gradient::kMaxKeys;
    }

    template <class T>
    luabridge::LuaRef keyTable(lua_State* L, const std::vector<T>& keys){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < keys.size(); i++){
            table[(int)i + 1] = keys[i];
        }
        return table;
    }

    void registerCurveClasses(lua_State* L){
        luabridge::getGlobalNamespace(L)
            .beginNamespace("WrapMode")
            .addVariable("Clamp", CurveWrapMode::Clamp)
            .addVariable("ClampForever", CurveWrapMode::Clamp)
            .addVariable("Constant", CurveWrapMode::Clamp)
            .addVariable("Loop", CurveWrapMode::Loop)
            .addVariable("PingPong", CurveWrapMode::PingPong)
            .addVariable("Linear", CurveWrapMode::Linear)
            .addVariable("LoopWithOffset", CurveWrapMode::LoopWithOffset)
            .addVariable("CycleWithOffset", CurveWrapMode::LoopWithOffset)
            .endNamespace();

        luabridge::getGlobalNamespace(L)
            .beginNamespace("TangentMode")
            .addVariable("Free", CurveTangentMode::Free)
            .addVariable("Auto", CurveTangentMode::Auto)
            .addVariable("Linear", CurveTangentMode::Linear)
            .addVariable("Constant", CurveTangentMode::Constant)
            .addVariable("ClampedAuto", CurveTangentMode::ClampedAuto)
            .endNamespace();

        luabridge::getGlobalNamespace(L)
            .beginNamespace("GradientMode")
            .addVariable("Blend", GradientMode::Blend)
            .addVariable("Fixed", GradientMode::Fixed)
            .endNamespace();

        luabridge::getGlobalNamespace(L)
            .beginNamespace("MinMaxCurveMode")
            .addVariable("Constant", particles::CurveMode::Constant)
            .addVariable("TwoConstants", particles::CurveMode::TwoConstants)
            .addVariable("Curve", particles::CurveMode::Curve)
            .addVariable("TwoCurves", particles::CurveMode::TwoCurves)
            .endNamespace();

        luabridge::getGlobalNamespace(L)
            .beginNamespace("MinMaxGradientMode")
            .addVariable("Color", particles::GradientValueMode::Color)
            .addVariable("Gradient", particles::GradientValueMode::Gradient)
            .addVariable("TwoColors", particles::GradientValueMode::TwoColors)
            .addVariable("TwoGradients", particles::GradientValueMode::TwoGradients)
            .addVariable("RandomColor", particles::GradientValueMode::RandomColor)
            .endNamespace();

        luabridge::getGlobalNamespace(L)
            .beginClass<CurveKey>("Keyframe")
            .addConstructor(
                [](void* ptr) { return new (ptr) CurveKey(); },
                [](void* ptr, float time, float value) { return new (ptr) CurveKey{time, value, 0.0f, 0.0f}; },
                [](void* ptr, float time, float value, float inTangent, float outTangent) {
                    return new (ptr) CurveKey{time, value, inTangent, outTangent}; })
            .addProperty("time", &CurveKey::time, &CurveKey::time)
            .addProperty("value", &CurveKey::value, &CurveKey::value)
            .addProperty("inTangent", &CurveKey::inTangent, &CurveKey::inTangent)
            .addProperty("outTangent", &CurveKey::outTangent, &CurveKey::outTangent)
            .addProperty("inTangentMode", &CurveKey::inMode, &CurveKey::setInMode)
            .addProperty("outTangentMode", &CurveKey::outMode, &CurveKey::setOutMode)
            .addProperty("broken", &CurveKey::broken, &CurveKey::setBroken)
            .addFunction("__eq", &equalsLua<CurveKey>)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<Curve>("AnimationCurve")
            .addConstructor<void()>()
            .addStaticFunction("linear", [](float timeStart, float valueStart, float timeEnd, float valueEnd) -> Curve {
                return Curve::linear(timeStart, valueStart, timeEnd, valueEnd); })
            .addStaticFunction("easeInOut", &Curve::easeInOut)
            // Unity's constant(timeStart, timeEnd, value); constant(value) is one key (upstream's form)
            .addStaticFunction("constant", [](float first, std::optional<float> timeEnd, std::optional<float> value) -> Curve {
                if (!timeEnd || !value) return Curve::constant(first);
                return Curve::constant(first, *timeEnd, *value); })
            .addFunction("evaluate", &Curve::evaluate)
            .addFunction("addKey",
                [](Curve& curve, float time, float value) -> int { return luaIndex(curve.addKey(time, value)); },
                [](Curve& curve, float time, float value, float inTangent, float outTangent) -> int {
                    return luaIndex(curve.addKey(CurveKey{time, value, inTangent, outTangent})); },
                [](Curve& curve, const CurveKey& key) -> int { return luaIndex(curve.addKey(key)); })
            .addFunction("removeKey", [](Curve& curve, int index) -> bool {
                if (!keyIndex(curve, index)) return false;
                curve.removeKey((size_t)index - 1);
                return true; })
            .addFunction("moveKey",
                [](Curve& curve, int index, const CurveKey& key) -> int {
                    if (!keyIndex(curve, index)) return -1;
                    return luaIndex(curve.moveKey((size_t)index - 1, key)); },
                // the key to (time, value), its tangents kept
                [](Curve& curve, int index, float time, float value) -> int {
                    if (!keyIndex(curve, index)) return -1;
                    CurveKey key = curve.keys[(size_t)index - 1];
                    key.time = time;
                    key.value = value;
                    return luaIndex(curve.moveKey((size_t)index - 1, key)); })
            .addFunction("getKey", [](const Curve& curve, int index) -> std::optional<CurveKey> {
                if (!keyIndex(curve, index)) return std::nullopt;
                return curve.keys[(size_t)index - 1]; })
            .addFunction("smoothTangents", [](Curve& curve, int index, float weight) -> bool {
                if (!keyIndex(curve, index)) return false;
                curve.smoothTangents((size_t)index - 1, weight);
                return true; })
            .addProperty("length", [](const Curve* curve) -> int { return (int)curve->keys.size(); })
            .addProperty("keyCount", [](const Curve* curve) -> int { return (int)curve->keys.size(); })
            // every key (a copy, a 1-based table); set it to replace them (sorted, equal times dropped)
            .addProperty("keys",
                [](const Curve* curve, lua_State* L) -> luabridge::LuaRef { return keyTable(L, curve->keys); },
                [](Curve* curve, const luabridge::LuaRef& keys, lua_State*) {
                    std::vector<CurveKey> read;
                    if (keys.isTable()){
                        for (int i = 1; i <= keys.length(); i++){
                            const luabridge::LuaRef entry = keys[i];
                            if (!entry.isInstance<CurveKey>()) continue;
                            if (const auto key = entry.cast<CurveKey>()) read.push_back(*key);
                        }
                    }
                    curve->keys = std::move(read);
                    curve->normalize(); })
            .addProperty("preWrapMode", &Curve::preWrapMode, &Curve::preWrapMode)
            .addProperty("postWrapMode", &Curve::postWrapMode, &Curve::postWrapMode)
            .addFunction("__eq", &equalsLua<Curve>)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<GradientColorKey>("GradientColorKey")
            .addConstructor(
                [](void* ptr) { return new (ptr) GradientColorKey(); },
                [](void* ptr, Vector3 color, float time) { return new (ptr) GradientColorKey{time, color}; })
            .addProperty("color", &GradientColorKey::color, &GradientColorKey::color)
            .addProperty("time", &GradientColorKey::time, &GradientColorKey::time)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<GradientAlphaKey>("GradientAlphaKey")
            .addConstructor(
                [](void* ptr) { return new (ptr) GradientAlphaKey(); },
                [](void* ptr, float alpha, float time) { return new (ptr) GradientAlphaKey{time, alpha}; })
            .addProperty("alpha", &GradientAlphaKey::alpha, &GradientAlphaKey::alpha)
            .addProperty("time", &GradientAlphaKey::time, &GradientAlphaKey::time)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<Gradient>("Gradient")
            .addConstructor<void()>()
            .addFunction("evaluate", &Gradient::evaluate)
            // false (and the gradient unchanged, a warning logged) for more than 8 keys of a kind
            .addFunction("setKeys", [](Gradient& gradient, const luabridge::LuaRef& colorKeys, const luabridge::LuaRef& alphaKeys) -> bool {
                std::vector<GradientColorKey> colors;
                std::vector<GradientAlphaKey> alphas;
                if (!keyList(colorKeys, colors) || !keyList(alphaKeys, alphas)){
                    Log::warn("Gradient:setKeys: at most %d colour and %d alpha keys (got %d and %d): nothing set",
                              (int)Gradient::kMaxKeys, (int)Gradient::kMaxKeys, (int)colors.size(), (int)alphas.size());
                    return false;
                }
                gradient.colorKeys = std::move(colors);
                gradient.alphaKeys = std::move(alphas);
                gradient.normalize();
                return true; })
            .addProperty("colorKeyCount", [](const Gradient* gradient) -> int { return (int)gradient->colorKeys.size(); })
            .addProperty("alphaKeyCount", [](const Gradient* gradient) -> int { return (int)gradient->alphaKeys.size(); })
            // a copy of colour / alpha key `index` (1-based), nil out of range
            .addFunction("getColorKey", [](const Gradient& gradient, int index) -> std::optional<GradientColorKey> {
                if (index < 1 || (size_t)index > gradient.colorKeys.size()) return std::nullopt;
                return gradient.colorKeys[(size_t)index - 1]; })
            .addFunction("getAlphaKey", [](const Gradient& gradient, int index) -> std::optional<GradientAlphaKey> {
                if (index < 1 || (size_t)index > gradient.alphaKeys.size()) return std::nullopt;
                return gradient.alphaKeys[(size_t)index - 1]; })
            .addProperty("colorKeys", [](const Gradient* gradient, lua_State* L) -> luabridge::LuaRef {
                return keyTable(L, gradient->colorKeys); })
            .addProperty("alphaKeys", [](const Gradient* gradient, lua_State* L) -> luabridge::LuaRef {
                return keyTable(L, gradient->alphaKeys); })
            .addProperty("mode", &Gradient::mode, &Gradient::mode)
            .addProperty("linear", &Gradient::linear, &Gradient::linear)
            .addFunction("__eq", &equalsLua<Gradient>)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<particles::MinMaxCurve>("MinMaxCurve")
            // origin/main's (Unity's) constructors too: (min, max) two constants, (multiplier,
            // curve) a curve, (multiplier, min curve, max curve) two curves
            .addConstructor(
                [](void* ptr) { return new (ptr) particles::MinMaxCurve(); },
                [](void* ptr, float value) { return new (ptr) particles::MinMaxCurve(value); },
                [](void* ptr, float lo, float hi) { return new (ptr) particles::MinMaxCurve(particles::MinMaxCurve::twoConstants(lo, hi)); },
                [](void* ptr, float multiplier, const Curve& curve) { return new (ptr) particles::MinMaxCurve(particles::MinMaxCurve::fromCurve(curve, multiplier)); },
                [](void* ptr, float multiplier, const Curve& curveMin, const Curve& curveMax) {
                    particles::MinMaxCurve value = particles::MinMaxCurve::fromCurve(curveMin, multiplier);
                    value.mode = particles::CurveMode::TwoCurves;
                    value.curveMax = curveMax;
                    return new (ptr) particles::MinMaxCurve(value); })
            .addStaticFunction("twoConstants", &particles::MinMaxCurve::twoConstants)
            .addStaticFunction("fromCurve", [](const Curve& curve, float multiplier) -> particles::MinMaxCurve {
                return particles::MinMaxCurve::fromCurve(curve, multiplier); })
            .addStaticFunction("twoCurves", [](const Curve& curveMin, const Curve& curveMax, float multiplier) -> particles::MinMaxCurve {
                particles::MinMaxCurve value = particles::MinMaxCurve::fromCurve(curveMin, multiplier);
                value.mode = particles::CurveMode::TwoCurves;
                value.curveMax = curveMax;
                return value; })
            .addFunction("evaluate", [](const particles::MinMaxCurve& value, float time, std::optional<float> random) -> float {
                return value.evaluate(time, random.value_or(0.0f)); })
            .addProperty("mode", &particles::MinMaxCurve::mode, &particles::MinMaxCurve::mode)
            .addProperty("constant", &particles::MinMaxCurve::constant, &particles::MinMaxCurve::constant)
            .addProperty("constantMin", &particles::MinMaxCurve::constant, &particles::MinMaxCurve::constant)
            .addProperty("constantMax", &particles::MinMaxCurve::constantMax, &particles::MinMaxCurve::constantMax)
            .addProperty("curve", &particles::MinMaxCurve::curve, &particles::MinMaxCurve::curve)
            .addProperty("curveMin", &particles::MinMaxCurve::curve, &particles::MinMaxCurve::curve)
            .addProperty("curveMax", &particles::MinMaxCurve::curveMax, &particles::MinMaxCurve::curveMax)
            .addProperty("multiplier", &particles::MinMaxCurve::multiplier, &particles::MinMaxCurve::multiplier)
            .endClass();

        luabridge::getGlobalNamespace(L)
            .beginClass<particles::MinMaxGradient>("MinMaxGradient")
            // origin/main's (Unity's) constructors too (sRGB colours, as the pickers): a colour
            // (Vector3: opaque), two colours, a gradient, two gradients
            .addConstructor(
                [](void* ptr) { return new (ptr) particles::MinMaxGradient(); },
                [](void* ptr, Vector4 color) { return new (ptr) particles::MinMaxGradient(color); },
                [](void* ptr, Vector3 color) { return new (ptr) particles::MinMaxGradient(Vector4(color.x, color.y, color.z, 1.0f)); },
                [](void* ptr, Vector4 lo, Vector4 hi) {
                    particles::MinMaxGradient value(lo);
                    value.mode = particles::GradientValueMode::TwoColors;
                    value.colorMax = hi;
                    return new (ptr) particles::MinMaxGradient(value); },
                [](void* ptr, const Gradient& gradient) {
                    particles::MinMaxGradient value;
                    value.mode = particles::GradientValueMode::Gradient;
                    value.gradient = gradient;
                    return new (ptr) particles::MinMaxGradient(value); },
                [](void* ptr, const Gradient& lo, const Gradient& hi) {
                    particles::MinMaxGradient value;
                    value.mode = particles::GradientValueMode::TwoGradients;
                    value.gradient = lo;
                    value.gradientMax = hi;
                    return new (ptr) particles::MinMaxGradient(value); })
            .addFunction("evaluate", [](const particles::MinMaxGradient& value, float time, std::optional<float> random) -> Vector4 {
                return value.evaluate(time, random.value_or(0.0f)); })
            .addProperty("mode", &particles::MinMaxGradient::mode, &particles::MinMaxGradient::mode)
            .addProperty("color", &particles::MinMaxGradient::color, &particles::MinMaxGradient::color)
            .addProperty("colorMin", &particles::MinMaxGradient::color, &particles::MinMaxGradient::color)
            .addProperty("colorMax", &particles::MinMaxGradient::colorMax, &particles::MinMaxGradient::colorMax)
            .addProperty("gradient", &particles::MinMaxGradient::gradient, &particles::MinMaxGradient::gradient)
            .addProperty("gradientMin", &particles::MinMaxGradient::gradient, &particles::MinMaxGradient::gradient)
            .addProperty("gradientMax", &particles::MinMaxGradient::gradientMax, &particles::MinMaxGradient::gradientMax)
            .endClass();

        // upstream's (lines and trails) names: CurveKey is Keyframe, CurveWrap is WrapMode (its
        // Clamp, Loop and PingPong are the same values)
        lua_getglobal(L, "Keyframe");
        lua_setglobal(L, "CurveKey");
        lua_getglobal(L, "WrapMode");
        lua_setglobal(L, "CurveWrap");
    }

}

void LuaBinding::registerMathClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("RotationOrder")
        .addVariable("XYZ", RotationOrder::XYZ)
        .addVariable("XZY", RotationOrder::XZY)
        .addVariable("YXZ", RotationOrder::YXZ)
        .addVariable("YZX", RotationOrder::YZX)
        .addVariable("ZXY", RotationOrder::ZXY)
        .addVariable("ZYX", RotationOrder::ZYX)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<Vector2>("Vector2")
        .addConstructor<
            void(), 
            void(const float, const float), 
            void(Vector2),
            void(Vector3), 
            void(Vector4)>()
        .addStaticProperty("ZERO", &constantCopy<Vector2, Vector2::ZERO>)
        .addStaticProperty("UNIT_X", &constantCopy<Vector2, Vector2::UNIT_X>)
        .addStaticProperty("UNIT_Y", &constantCopy<Vector2, Vector2::UNIT_Y>)
        .addStaticProperty("NEGATIVE_UNIT_X", &constantCopy<Vector2, Vector2::NEGATIVE_UNIT_X>)
        .addStaticProperty("NEGATIVE_UNIT_Y", &constantCopy<Vector2, Vector2::NEGATIVE_UNIT_Y>)
        .addStaticProperty("UNIT_SCALE", &constantCopy<Vector2, Vector2::UNIT_SCALE>)
        .addProperty("x", &Vector2::x, &Vector2::x)
        .addProperty("y", &Vector2::y, &Vector2::y)
        .addFunction("__tostring", &Vector2::toString)
        .addFunction("__eq", &equalsLua<Vector2>)
        .addFunction("__lt", &Vector2::operator<)
        .addFunction("__le", [](const Vector2& v, const Vector2& other) -> bool {
            return v.x <= other.x && v.y <= other.y; })
        .addFunction("__sub", (Vector2 (Vector2::*)(const Vector2&) const)&Vector2::operator-)
        .addFunction("__add", (Vector2 (Vector2::*)(const Vector2&) const)&Vector2::operator+)
        .addFunction("__div", 
            luabridge::overload<float>(&Vector2::operator/),
            luabridge::overload<const Vector2&>(&Vector2::operator/))
        .addFunction("__mul", 
            luabridge::overload<float>(&Vector2::operator*),
            luabridge::overload<const Vector2&>(&Vector2::operator*))
        .addFunction("__unm", (Vector2 (Vector2::*)() const)&Vector2::operator-)
        .addFunction("isValid", &Vector2::isValid)
        .addFunction("swap", &Vector2::swap)
        .addFunction("length", &Vector2::length)
        .addFunction("squaredLength", &Vector2::squaredLength)
        .addFunction("distance", &Vector2::distance)
        .addFunction("squaredDistance", &Vector2::squaredDistance)
        .addFunction("dotProduct", &Vector2::dotProduct)
        .addFunction("normalize", &Vector2::normalize)
        .addFunction("normalized", &Vector2::normalized)
        .addFunction("normalizeL", &Vector2::normalizeL)
        .addFunction("midPoint", &Vector2::midPoint)
        .addFunction("moveTowards", &Vector2::moveTowards)
        .addFunction("lerp", &Vector2::lerp)
        .addFunction("lerpUnclamped", &Vector2::lerpUnclamped)
        .addFunction("makeFloor", &Vector2::makeFloor)
        .addFunction("makeCeil", &Vector2::makeCeil)
        .addFunction("perpendicular", &Vector2::perpendicular)
        .addFunction("crossProduct", &Vector2::crossProduct)
        .addFunction("normalizedCopy", &Vector2::normalizedCopy)
        .addFunction("reflect", &Vector2::reflect)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Vector3>("Vector3")
        .addConstructor<
            void(), 
            void(const float, const float, const float), 
            void(Vector2, const float),
            void(Vector3), 
            void(Vector4)>()
        .addStaticProperty("ZERO", &constantCopy<Vector3, Vector3::ZERO>)
        .addStaticProperty("UNIT_X", &constantCopy<Vector3, Vector3::UNIT_X>)
        .addStaticProperty("UNIT_Y", &constantCopy<Vector3, Vector3::UNIT_Y>)
        .addStaticProperty("UNIT_Z", &constantCopy<Vector3, Vector3::UNIT_Z>)
        .addStaticProperty("UNIT_SCALE", &constantCopy<Vector3, Vector3::UNIT_SCALE>)
        .addProperty("x", &Vector3::x, &Vector3::x)
        .addProperty("y", &Vector3::y, &Vector3::y)
        .addProperty("z", &Vector3::z, &Vector3::z)
        .addFunction("__tostring", &Vector3::toString)
        .addFunction("__eq", &equalsLua<Vector3>)
        .addFunction("__lt", &Vector3::operator<)
        .addFunction("__le", [](const Vector3& v, const Vector3& other) -> bool {
            return v.x <= other.x && v.y <= other.y && v.z <= other.z; })
        .addFunction("__sub", (Vector3 (Vector3::*)(const Vector3&) const)&Vector3::operator-)
        .addFunction("__add", (Vector3 (Vector3::*)(const Vector3&) const)&Vector3::operator+)
        .addFunction("__div",
            luabridge::overload<float>(&Vector3::operator/),
            // component-wise, as __mul (Vector2 and Vector4 declare it, Vector3 does not)
            [](const Vector3& v, const Vector3& divisor) -> Vector3 {
                return Vector3(v.x / divisor.x, v.y / divisor.y, v.z / divisor.z); })
        .addFunction("__mul", 
            luabridge::overload<float>(&Vector3::operator*), // need float operator first to fix estrange error in Emscripten
            luabridge::overload<const Vector3&>(&Vector3::operator*))
        .addFunction("__unm", (Vector3 (Vector3::*)() const)&Vector3::operator-)
        .addFunction("isValid", &Vector3::isValid)
        .addFunction("length", &Vector3::length)
        .addFunction("squaredLength", &Vector3::squaredLength)
        .addFunction("dotProduct", &Vector3::dotProduct)
        .addFunction("absDotProduct", &Vector3::absDotProduct)
        .addFunction("distance", &Vector3::distance)
        .addFunction("squaredDistance", &Vector3::squaredDistance)
        .addFunction("normalize", &Vector3::normalize)
        .addFunction("normalized", &Vector3::normalized)
        .addFunction("normalizeL", &Vector3::normalizeL)
        .addFunction("crossProduct", &Vector3::crossProduct)
        .addFunction("midPoint", &Vector3::midPoint)
        .addFunction("moveTowards", &Vector3::moveTowards)
        .addFunction("lerp", &Vector3::lerp)
        .addFunction("lerpUnclamped", &Vector3::lerpUnclamped)
        .addFunction("makeFloor", &Vector3::makeFloor)
        .addFunction("makeCeil", &Vector3::makeCeil)
        .addFunction("perpendicular", &Vector3::perpendicular)
        .addFunction("reflect", &Vector3::reflect)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Vector4>("Vector4")
        .addConstructor<
            void(), 
            void(const float, const float, const float, const float), 
            void(Vector2, const float, const float),
            void(Vector3, const float), 
            void(Vector4)>()
        .addStaticProperty("ZERO", &constantCopy<Vector4, Vector4::ZERO>)
        .addStaticProperty("UNIT_X", &constantCopy<Vector4, Vector4::UNIT_X>)
        .addStaticProperty("UNIT_Y", &constantCopy<Vector4, Vector4::UNIT_Y>)
        .addStaticProperty("UNIT_Z", &constantCopy<Vector4, Vector4::UNIT_Z>)
        .addStaticProperty("UNIT_W", &constantCopy<Vector4, Vector4::UNIT_W>)
        .addStaticProperty("UNIT_SCALE", &constantCopy<Vector4, Vector4::UNIT_SCALE>)
        .addProperty("x", &Vector4::x, &Vector4::x)
        .addProperty("y", &Vector4::y, &Vector4::y)
        .addProperty("z", &Vector4::z, &Vector4::z)
        .addProperty("w", &Vector4::w, &Vector4::w)
        .addFunction("__tostring", &Vector4::toString)
        .addFunction("__eq", &equalsLua<Vector4>)
        .addFunction("__lt", &Vector4::operator<)
        .addFunction("__le", [](const Vector4& v, const Vector4& other) -> bool {
            return v.x <= other.x && v.y <= other.y && v.z <= other.z && v.w <= other.w; })
        .addFunction("__sub", (Vector4 (Vector4::*)(const Vector4&) const)&Vector4::operator-)
        .addFunction("__add", (Vector4 (Vector4::*)(const Vector4&) const)&Vector4::operator+)
        .addFunction("__div", 
            luabridge::overload<float>(&Vector4::operator/),
            luabridge::overload<const Vector4&>(&Vector4::operator/))
        .addFunction("__mul", 
            luabridge::overload<float>(&Vector4::operator*),
            luabridge::overload<const Vector4&>(&Vector4::operator*))
        .addFunction("__unm", (Vector4 (Vector4::*)() const)&Vector4::operator-)
        .addFunction("isValid", &Vector4::isValid)
        .addFunction("swap", &Vector4::swap)
        .addFunction("divideByW", &Vector4::divideByW)
        .addFunction("dotProduct", &Vector4::dotProduct)
        .addFunction("moveTowards", &Vector4::moveTowards)
        .addFunction("lerp", &Vector4::lerp)
        .addFunction("lerpUnclamped", &Vector4::lerpUnclamped)
        .addFunction("isNaN", &Vector4::isNaN)
        .endClass();


    luabridge::getGlobalNamespace(L)
        .beginClass<Rect>("Rect")
        .addConstructor<void(), void(float, float, float, float)>()
        .addProperty("x", &Rect::getX, &Rect::setX)
        .addProperty("y", &Rect::getY, &Rect::setY)
        .addProperty("width", &Rect::getWidth, &Rect::setWidth)
        .addProperty("height", &Rect::getHeight, &Rect::setHeight)
        .addFunction("__tostring", &Rect::toString)
        .addFunction("__eq", &equalsLua<Rect>)
        .addFunction("getVector", &Rect::getVector)
        .addFunction("setRect", 
            luabridge::overload<Rect>(&Rect::setRect),
            luabridge::overload<float, float, float, float>(&Rect::setRect))
        .addFunction("fitOnRect", &Rect::fitOnRect)
        .addFunction("contains", &Rect::contains)
        .addFunction("isNormalized", &Rect::isNormalized)
        .addFunction("isZero", &Rect::isZero)
        .endClass();

    // Distances in from the four edges of a rectangle (Screen.safeAreaInsets, Screen.extraInsets)
    luabridge::getGlobalNamespace(L)
        .beginClass<EdgeInsets>("EdgeInsets")
        .addConstructor<void(), void(float, float, float, float)>()
        .addProperty("left", &EdgeInsets::left, &EdgeInsets::left)
        .addProperty("top", &EdgeInsets::top, &EdgeInsets::top)
        .addProperty("right", &EdgeInsets::right, &EdgeInsets::right)
        .addProperty("bottom", &EdgeInsets::bottom, &EdgeInsets::bottom)
        .addFunction("__tostring", &EdgeInsets::toString)
        .addFunction("__eq", &equalsLua<EdgeInsets>)
        .addFunction("isZero", &EdgeInsets::isZero)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Sphere>("Sphere")
        .addConstructor<void(), void(Vector3, float)>()
        .addProperty("center", &Sphere::center, &Sphere::center)
        .addProperty("radius", &Sphere::radius, &Sphere::radius)
        .addFunction("toString", &Sphere::toString)
        .addFunction("contains", &Sphere::contains)
        .addFunction("intersects", 
            luabridge::overload<const Sphere&>(&Sphere::intersects),
            luabridge::overload<const AABB&>(&Sphere::intersects),
            luabridge::overload<const OBB&>(&Sphere::intersects),
            luabridge::overload<const Plane&>(&Sphere::intersects),
            luabridge::overload<const Vector3&>(&Sphere::intersects))
        .addFunction("merge", &Sphere::merge)
        .addFunction("surfaceArea", &Sphere::surfaceArea)
        .addFunction("volume", &Sphere::volume)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Matrix3>("Matrix3")
        .addConstructor<
            void(), 
            void(
                float, float, float, 
                float, float, float, 
                float, float, float)>()
        .addFunction("__tostring", &Matrix3::toString)
        .addFunction("__eq", &equalsLua<Matrix3>)
        .addFunction("__sub", (Matrix3 (Matrix3::*)(const Matrix3&) const)&Matrix3::operator-)
        .addFunction("__add", (Matrix3 (Matrix3::*)(const Matrix3&) const)&Matrix3::operator+)
        .addFunction("__mul",
            luabridge::overload<float>(&Matrix3::operator*),
            luabridge::overload<const Vector3&>(&Matrix3::operator*),
            luabridge::overload<const Matrix3&>(&Matrix3::operator*))
        .addFunction("row", &Matrix3::row)
        .addFunction("column", &Matrix3::column)
        .addFunction("set", &Matrix3::set)
        .addFunction("get", &Matrix3::get)
        .addFunction("setRow", &Matrix3::setRow)
        .addFunction("setColumn", &Matrix3::setColumn)
        .addFunction("isValid", &Matrix3::isValid)
        .addFunction("identity", &Matrix3::identity)
        .addFunction("calcInverse", &Matrix3::calcInverse)
        .addFunction("inverse", &Matrix3::inverse)
        .addFunction("transpose", &Matrix3::transpose)
        .addStaticFunction("rotateMatrix", 
            luabridge::overload<const float, const Vector3&>(&Matrix3::rotateMatrix),
            luabridge::overload<const float, const float>(&Matrix3::rotateMatrix))
        .addStaticFunction("rotateXMatrix", &Matrix3::rotateXMatrix)
        .addStaticFunction("rotateYMatrix", &Matrix3::rotateYMatrix)
        .addStaticFunction("rotateZMatrix", &Matrix3::rotateZMatrix)
        .addStaticFunction("scaleMatrix", 
            luabridge::overload<const Vector3&>(&Matrix3::scaleMatrix),
            luabridge::overload<const float>(&Matrix3::scaleMatrix))
        .addFunction("decomposeQDU", &Matrix3::decomposeQDU)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Matrix4>("Matrix4")
        .addConstructor<
            void(), 
            void(
                float, float, float, float,
                float, float, float, float,
                float, float, float, float,
                float, float, float, float)>()
        .addFunction("__tostring", &Matrix4::toString)
        .addFunction("__eq", &equalsLua<Matrix4>)
        .addFunction("__sub", (Matrix4 (Matrix4::*)(const Matrix4&) const)&Matrix4::operator-)
        .addFunction("__add", (Matrix4 (Matrix4::*)(const Matrix4&) const)&Matrix4::operator+)
        .addFunction("__mul",
            luabridge::overload<float>(&Matrix4::operator*),
            luabridge::overload<const Vector3&>(&Matrix4::operator*),
            luabridge::overload<const Vector4&>(&Matrix4::operator*),
            luabridge::overload<const Matrix4&>(&Matrix4::operator*))
        .addFunction("row", &Matrix4::row)
        .addFunction("column", &Matrix4::column)
        .addFunction("set", &Matrix4::set)
        .addFunction("get", &Matrix4::get)
        .addFunction("setRow", &Matrix4::setRow)
        .addFunction("setColumn", &Matrix4::setColumn)
        .addFunction("isValid", &Matrix4::isValid)
        .addFunction("identity", &Matrix4::identity)
        .addFunction("translateInPlace", &Matrix4::translateInPlace)
        .addFunction("linear", &Matrix4::linear)
        .addFunction("inverse", &Matrix4::inverse)
        .addFunction("transpose", &Matrix4::transpose)
        .addFunction("determinant", &Matrix4::determinant)
        .addStaticFunction("translateMatrix", 
            luabridge::overload<const Vector3&>(&Matrix4::translateMatrix),
            luabridge::overload<const float, const float, const float>(&Matrix4::translateMatrix))
        .addStaticFunction("rotateMatrix", 
            luabridge::overload<const float, const Vector3&>(&Matrix4::rotateMatrix),
            luabridge::overload<const float, const float>(&Matrix4::rotateMatrix))
        .addStaticFunction("rotateXMatrix", &Matrix4::rotateXMatrix)
        .addStaticFunction("rotateYMatrix", &Matrix4::rotateYMatrix)
        .addStaticFunction("rotateZMatrix", &Matrix4::rotateZMatrix)
        .addStaticFunction("scaleMatrix", 
            luabridge::overload<const Vector3&>(&Matrix4::scaleMatrix),
            luabridge::overload<const float>(&Matrix4::scaleMatrix))
        .addStaticFunction("lookAtMatrix", &Matrix4::lookAtMatrix)
        .addStaticFunction("frustumMatrix", &Matrix4::frustumMatrix)
        .addStaticFunction("orthoMatrix", &Matrix4::orthoMatrix)
        .addStaticFunction("perspectiveMatrix", &Matrix4::perspectiveMatrix)
        .addFunction("decomposeStandard", &Matrix4::decomposeStandard)
        .addFunction("decomposeQDU", &Matrix4::decomposeQDU)
        .addFunction("decompose", &Matrix4::decompose)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Quaternion>("Quaternion")
        .addConstructor<
            void(), 
            void(const float, const float, const float, const float), 
            void(const float, const float, const float),
            void(const float, const float, const float, const RotationOrder&),
            void(const Vector3*),
            void(const Vector3&, const Vector3&, const Vector3&),
            void(const float, const Vector3&),
            void(const Matrix3&),
            void(const Matrix4&)>()
        .addStaticProperty("IDENTITY", &constantCopy<Quaternion, Quaternion::IDENTITY>)
        .addProperty("w", &Quaternion::w, &Quaternion::w)
        .addProperty("x", &Quaternion::x, &Quaternion::x)
        .addProperty("y", &Quaternion::y, &Quaternion::y)
        .addProperty("z", &Quaternion::z, &Quaternion::z)
        .addFunction("__tostring", &Quaternion::toString)
        .addFunction("__eq", &equalsLua<Quaternion>)
        .addFunction("__sub", (Quaternion (Quaternion::*)(const Quaternion&) const)&Quaternion::operator-)
        .addFunction("__add", (Quaternion (Quaternion::*)(const Quaternion&) const)&Quaternion::operator+)
        .addFunction("__mul",
            luabridge::overload<float>(&Quaternion::operator*),
            luabridge::overload<const Quaternion&>(&Quaternion::operator*),
            luabridge::overload<const Vector3&>(&Quaternion::operator*))
        .addFunction("__unm", (Quaternion (Quaternion::*)() const)&Quaternion::operator-)
        .addFunction("fromEulerAngles", &Quaternion::fromEulerAngles)
        .addFunction("fromAxes", 
            luabridge::overload<const Vector3*>(&Quaternion::fromAxes),
            luabridge::overload<const Vector3&, const Vector3&, const Vector3&>(&Quaternion::fromAxes))
        .addFunction("fromRotationMatrix", 
            luabridge::overload<const Matrix3&>(&Quaternion::fromRotationMatrix),
            luabridge::overload<const Matrix4&>(&Quaternion::fromRotationMatrix))
        .addFunction("getRotationMatrix", &Quaternion::getRotationMatrix)
        .addFunction("fromAngle", &Quaternion::fromAngle)
        .addFunction("fromAngleAxis", &Quaternion::fromAngleAxis)
        .addFunction("getEulerAngles", &Quaternion::getEulerAngles)
        .addFunction("xAxis", &Quaternion::xAxis)
        .addFunction("yAxis", &Quaternion::yAxis)
        .addFunction("zAxis", &Quaternion::zAxis)
        .addFunction("dot", &Quaternion::dot)
        .addFunction("norm", &Quaternion::norm)
        .addFunction("inverse", &Quaternion::inverse)
        .addFunction("unitInverse", &Quaternion::unitInverse)
        .addFunction("exp", &Quaternion::exp)
        .addFunction("log", &Quaternion::log)
        .addStaticFunction("slerp", 
            luabridge::overload<float, const Quaternion&, const Quaternion&>(&Quaternion::slerp),
            luabridge::overload<float, const Quaternion&, const Quaternion&, bool>(&Quaternion::slerp))
        .addStaticFunction("slerpExtraSpins", &Quaternion::slerpExtraSpins)
        .addStaticFunction("nlerp", 
            luabridge::overload<float, const Quaternion&, const Quaternion&>(&Quaternion::nlerp),
            luabridge::overload<float, const Quaternion&, const Quaternion&, bool>(&Quaternion::nlerp))
        .addStaticFunction("squad", &Quaternion::squad)
        .addStaticFunction("lookRotation",
            luabridge::overload<const Vector3&>(&Quaternion::lookRotation),
            luabridge::overload<const Vector3&, const Vector3&>(&Quaternion::lookRotation))
        .addFunction("normalize", &Quaternion::normalize)
        .addFunction("normalized", &Quaternion::normalized)
        .addFunction("normalizeL", &Quaternion::normalizeL)
        .addFunction("getRoll", &Quaternion::getRoll)
        .addFunction("getPitch", &Quaternion::getPitch)
        .addFunction("getYaw", &Quaternion::getYaw)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Plane>("Plane")
        .addConstructor<
            void(), 
            void(const Vector3&, float), 
            void(float, float, float, float), 
            void(const Vector3&, const Vector3&), 
            void(const Vector3&, const Vector3&, const Vector3&)>()
        .addFunction("__unm", (Plane (Plane::*)() const)&Plane::operator-)
        .addFunction("__eq", &equalsLua<Plane>)
        .addFunction("getSide", 
            luabridge::overload<const Vector3&>(&Plane::getSide),
            luabridge::overload<const Vector3&, const Vector3&>(&Plane::getSide),
            luabridge::overload<const AABB&>(&Plane::getSide),
            luabridge::overload<const OBB&>(&Plane::getSide))
        .addFunction("getDistance", &Plane::getDistance)
        .addFunction("redefine", 
            luabridge::overload<const Vector3&, const Vector3&, const Vector3&>(&Plane::redefine),
            luabridge::overload<const Vector3&, const Vector3&>(&Plane::redefine))
        .addFunction("projectVector", &Plane::projectVector)
        .addFunction("normalize", &Plane::normalize)
        .addFunction("normalized", &Plane::normalized)
        .addStaticProperty("Side", [](lua_State* L) {
            auto table = luabridge::newTable(L);
            table.push(L);
            luabridge::getNamespaceFromStack(L)
                .addVariable("NO_SIDE", Plane::Side::NO_SIDE)
                .addVariable("POSITIVE_SIDE", Plane::Side::POSITIVE_SIDE)
                .addVariable("NEGATIVE_SIDE", Plane::Side::NEGATIVE_SIDE)
                .addVariable("BOTH_SIDE", Plane::Side::BOTH_SIDE);
            table.pop();
            return table;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AABB>("AABB")
        .addConstructor<
            void(), 
            void(AABB::BoxType), 
            void(const Vector3&, const Vector3&), 
            void(float, float, float, float, float, float)>()
        .addStaticProperty("ZERO", &constantCopy<AABB, AABB::ZERO>)
        .addProperty("minimum", (const Vector3&(AABB::*)() const)&AABB::getMinimum, (void(AABB::*)(const Vector3&))&AABB::setMinimum)
        .addFunction("setMinimum", (void(AABB::*)(float, float, float))&AABB::setMinimum)
        .addFunction("setMinimumX", &AABB::setMinimumX)
        .addFunction("setMinimumY", &AABB::setMinimumY)
        .addFunction("setMinimumZ", &AABB::setMinimumZ)
        .addProperty("maximum", (const Vector3&(AABB::*)() const)&AABB::getMaximum, (void(AABB::*)(const Vector3&))&AABB::setMaximum)
        .addFunction("setMaximum", (void(AABB::*)(float, float, float))&AABB::setMaximum)
        .addFunction("setMaximumX", &AABB::setMaximumX)
        .addFunction("setMaximumY", &AABB::setMaximumY)
        .addFunction("setMaximumZ", &AABB::setMaximumZ)
        .addFunction("setExtents", 
            luabridge::overload<const Vector3&, const Vector3&>(&AABB::setExtents),
            luabridge::overload<float, float, float, float, float, float>(&AABB::setExtents))
        .addFunction("getCorner", &AABB::getCorner)
        // copies in CornerEnum order (the pointer form gave Lua one corner of the box's cache);
        // a null or infinite box has none
        .addFunction("getCorners", [] (const AABB& self) -> std::vector<Vector3> {
            std::vector<Vector3> corners;
            if (!self.isFinite())
                return corners;
            corners.reserve(8);
            for (int i = 0; i < 8; i++)
                corners.push_back(self.getCorner(static_cast<AABB::CornerEnum>(i)));
            return corners;
        })
        .addFunction("merge", 
            luabridge::overload<const AABB&>(&AABB::merge),
            luabridge::overload<const Vector3&>(&AABB::merge))
        .addFunction("transform", &AABB::transform)
        .addFunction("setNull", &AABB::setNull)
        .addFunction("isNull", &AABB::isNull)
        .addFunction("setInfinite", &AABB::setInfinite)
        .addFunction("isInfinite", &AABB::isInfinite)
        .addFunction("setFinite", &AABB::setFinite)
        .addFunction("isFinite", &AABB::isFinite)
        .addFunction("intersects", 
            luabridge::overload<const AABB&>(&AABB::intersects),
            luabridge::overload<const OBB&>(&AABB::intersects),
            luabridge::overload<const Plane&>(&AABB::intersects),
            luabridge::overload<const Sphere&>(&AABB::intersects),
            luabridge::overload<const Vector3&>(&AABB::intersects))
        .addFunction("intersection", &AABB::intersection)
        .addFunction("volume", &AABB::volume)
        .addFunction("scale", &AABB::scale)
        .addFunction("getCenter", &AABB::getCenter)
        .addFunction("getSize", &AABB::getSize)
        .addFunction("getHalfSize", &AABB::getHalfSize)
        .addFunction("contains", 
            luabridge::overload<const AABB&>(&AABB::contains),
            luabridge::overload<const Vector3&>(&AABB::contains))
        .addFunction("squaredDistance", &AABB::squaredDistance)
        .addFunction("distance", &AABB::distance)
        .addStaticProperty("BoxType", [](lua_State* L) {
            auto table = luabridge::newTable(L);
            table.push(L);
            luabridge::getNamespaceFromStack(L)
                .addVariable("BOXTYPE_NULL", AABB::BoxType::BOXTYPE_NULL)
                .addVariable("BOXTYPE_FINITE", AABB::BoxType::BOXTYPE_FINITE)
                .addVariable("BOXTYPE_INFINITE", AABB::BoxType::BOXTYPE_INFINITE);
            table.pop();
            return table;
            })
        .addStaticProperty("CornerEnum", [](lua_State* L) {
            auto table = luabridge::newTable(L);
            table.push(L);
            luabridge::getNamespaceFromStack(L)
                .addVariable("FAR_LEFT_BOTTOM", AABB::CornerEnum::FAR_LEFT_BOTTOM)
                .addVariable("FAR_LEFT_TOP", AABB::CornerEnum::FAR_LEFT_TOP)
                .addVariable("FAR_RIGHT_TOP", AABB::CornerEnum::FAR_RIGHT_TOP)
                .addVariable("FAR_RIGHT_BOTTOM", AABB::CornerEnum::FAR_RIGHT_BOTTOM)
                .addVariable("NEAR_RIGHT_BOTTOM", AABB::CornerEnum::NEAR_RIGHT_BOTTOM)
                .addVariable("NEAR_LEFT_BOTTOM", AABB::CornerEnum::NEAR_LEFT_BOTTOM)
                .addVariable("NEAR_LEFT_TOP", AABB::CornerEnum::NEAR_LEFT_TOP)
                .addVariable("NEAR_RIGHT_TOP", AABB::CornerEnum::NEAR_RIGHT_TOP);
            table.pop();
            return table;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<OBB>("OBB")
        .addConstructor<
            void(), 
            void(OBB::BoxType), 
            void(const Vector3&, const Vector3&),
            void(const Vector3&, const Vector3&, const Vector3&, const Vector3&, const Vector3&)>()
        .addFunction("toString", &OBB::toString)
        .addFunction("setNull", &OBB::setNull)
        .addFunction("isNull", &OBB::isNull)
        .addFunction("setInfinite", &OBB::setInfinite)
        .addFunction("isInfinite", &OBB::isInfinite)
        .addFunction("setFinite", &OBB::setFinite)
        .addFunction("isFinite", &OBB::isFinite)
        .addProperty("center", &OBB::getCenter, &OBB::setCenter)
        .addProperty("halfExtents", &OBB::getHalfExtents, &OBB::setHalfExtents)
        .addFunction("getAxisX", &OBB::getAxisX)
        .addFunction("getAxisY", &OBB::getAxisY)
        .addFunction("getAxisZ", &OBB::getAxisZ)
        .addFunction("setAxes", 
            luabridge::overload<const Quaternion&>(&OBB::setAxes),
            luabridge::overload<const Vector3&, const Vector3&, const Vector3&>(&OBB::setAxes))
        .addProperty("orientation", &OBB::getOrientation, &OBB::setOrientation)
        .addFunction("transform", 
            luabridge::overload<const Matrix4&>(&OBB::transform),
            luabridge::overload<const Vector3&, const Quaternion&, const Vector3&>(&OBB::transform))
        .addFunction("toAABB", &OBB::toAABB)
        .addFunction("toMatrix", &OBB::toMatrix)
        .addFunction("getCorner", &OBB::getCorner)
        .addFunction("getCorners", [] (const OBB& self) -> std::vector<Vector3> {
            std::vector<Vector3> corners;
            if (self.isNull() || self.isInfinite())
                return corners;
            corners.reserve(8);
            for (int i = 0; i < 8; i++)
                corners.push_back(self.getCorner(static_cast<OBB::CornerEnum>(i)));
            return corners;
        })
        .addFunction("enclose", 
            luabridge::overload<const Vector3&>(&OBB::enclose),
            luabridge::overload<const OBB&>(&OBB::enclose))
        .addFunction("intersects", 
            luabridge::overload<const OBB&>(&OBB::intersects),
            luabridge::overload<const AABB&>(&OBB::intersects),
            luabridge::overload<const Sphere&>(&OBB::intersects),
            luabridge::overload<const Plane&>(&OBB::intersects))
        .addFunction("contains", 
            luabridge::overload<const Vector3&>(&OBB::contains),
            luabridge::overload<const OBB&>(&OBB::contains))
        .addFunction("squaredDistance", &OBB::squaredDistance)
        .addFunction("distance", &OBB::distance)
        .addFunction("volume", &OBB::volume)
        .addFunction("closestPoint", &OBB::closestPoint)
        .addStaticProperty("BoxType", [](lua_State* L) {
            auto table = luabridge::newTable(L);
            table.push(L);
            luabridge::getNamespaceFromStack(L)
                .addVariable("BOXTYPE_NULL", OBB::BoxType::BOXTYPE_NULL)
                .addVariable("BOXTYPE_FINITE", OBB::BoxType::BOXTYPE_FINITE)
                .addVariable("BOXTYPE_INFINITE", OBB::BoxType::BOXTYPE_INFINITE);
            table.pop();
            return table;
            })
        .addStaticProperty("CornerEnum", [](lua_State* L) {
            auto table = luabridge::newTable(L);
            table.push(L);
            luabridge::getNamespaceFromStack(L)
                .addVariable("FAR_LEFT_BOTTOM", OBB::CornerEnum::FAR_LEFT_BOTTOM)
                .addVariable("FAR_LEFT_TOP", OBB::CornerEnum::FAR_LEFT_TOP)
                .addVariable("FAR_RIGHT_TOP", OBB::CornerEnum::FAR_RIGHT_TOP)
                .addVariable("FAR_RIGHT_BOTTOM", OBB::CornerEnum::FAR_RIGHT_BOTTOM)
                .addVariable("NEAR_RIGHT_BOTTOM", OBB::CornerEnum::NEAR_RIGHT_BOTTOM)
                .addVariable("NEAR_LEFT_BOTTOM", OBB::CornerEnum::NEAR_LEFT_BOTTOM)
                .addVariable("NEAR_LEFT_TOP", OBB::CornerEnum::NEAR_LEFT_TOP)
                .addVariable("NEAR_RIGHT_TOP", OBB::CornerEnum::NEAR_RIGHT_TOP);
            table.pop();
            return table;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("RayFilter")
        .addVariable("BODY_2D", RayFilter::BODY_2D)
        .addVariable("BODY_3D", RayFilter::BODY_3D)
        .addVariable("BOUNDS", RayFilter::BOUNDS)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        // Raycast result, read-only on purpose
        .beginClass<RayReturn>("RayReturn")
        .addProperty("hit", &RayReturn::hit)
        .addProperty("distance", &RayReturn::distance)
        .addProperty("point", &RayReturn::point)
        .addProperty("normal", &RayReturn::normal)
        .addProperty("body", &RayReturn::body)
        .addProperty("colliderIndex", &RayReturn::colliderIndex)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Ray>("Ray")
        .addConstructor<void(), void(Vector3, Vector3)>()
        .addStaticProperty("NO_HIT", &constantCopy<RayReturn, Ray::NO_HIT>)
        .addProperty("origin", &Ray::getOrigin, &Ray::setOrigin)
        .addProperty("direction", &Ray::getDirection, &Ray::setDirection)
        .addFunction("getPoint", &Ray::getPoint)
        .addFunction("intersects", 
            luabridge::overload<const AABB&>(&Ray::intersects),
            luabridge::overload<const OBB&>(&Ray::intersects),
            luabridge::overload<const Plane&>(&Ray::intersects),
#ifdef JASPERENGINE_PHYSICS_2D
            luabridge::overload<const Rigidbody2D&>(&Ray::intersects),
            luabridge::overload<const Rigidbody2D&, size_t>(&Ray::intersects),
#endif
#ifdef JASPERENGINE_PHYSICS_3D
            luabridge::overload<const Rigidbody&>(&Ray::intersects),
            luabridge::overload<const Rigidbody&, size_t>(&Ray::intersects),
#endif
            luabridge::overload<Scene*, RayFilter>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, bool>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, uint16_t, uint16_t>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, bool, uint16_t, uint16_t>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, Entity>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, const std::vector<Entity>&>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, bool, uint16_t, uint16_t, Entity>(&Ray::intersects),
            luabridge::overload<Scene*, RayFilter, bool, uint16_t, uint16_t, const std::vector<Entity>&>(&Ray::intersects)
#ifdef JASPERENGINE_PHYSICS_3D
            ,
            luabridge::overload<Scene*, uint8_t>(&Ray::intersects),
            luabridge::overload<Scene*, uint8_t, uint16_t, uint16_t>(&Ray::intersects),
            luabridge::overload<Scene*, uint8_t, Entity>(&Ray::intersects),
            luabridge::overload<Scene*, uint8_t, const std::vector<Entity>&>(&Ray::intersects),
            luabridge::overload<Scene*, uint8_t, uint16_t, uint16_t, Entity>(&Ray::intersects),
            luabridge::overload<Scene*, uint8_t, uint16_t, uint16_t, const std::vector<Entity>&>(&Ray::intersects)
#endif
        )
        .endClass();

    registerCurveClasses(L);

    addNumberFirst<Vector2, &numberTimes<Vector2>>(L, "__mul");
    addNumberFirst<Vector2, &numberDividedBy<Vector2>>(L, "__div");
    addNumberFirst<Vector3, &numberTimes<Vector3>>(L, "__mul");
    addNumberFirst<Vector3, &numberDividedByVector3>(L, "__div");
    addNumberFirst<Vector4, &numberTimes<Vector4>>(L, "__mul");
    addNumberFirst<Vector4, &numberDividedBy<Vector4>>(L, "__div");
    addNumberFirst<Quaternion, &numberTimes<Quaternion>>(L, "__mul");
    addNumberFirst<Matrix3, &numberTimes<Matrix3>>(L, "__mul");
    addNumberFirst<Matrix4, &numberTimes<Matrix4>>(L, "__mul");

#endif //DISABLE_LUA_BINDINGS
}
