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
#include "Ray.h"
#include "Sphere.h"
#include "Plane.h"
#include "AABB.h"
#include "OBB.h"
#include "AnimationCurve.h"
#include "Gradient.h"
#include "util/Color.h"

#include <optional>

using namespace jasperengine;

namespace luabridge
{
    template<> struct Stack<Plane::Side> : EnumWrapper<Plane::Side>{};
    template<> struct Stack<CurveWrap> : EnumWrapper<CurveWrap>{};
    template<> struct Stack<GradientMode> : EnumWrapper<GradientMode>{};
    template<> struct Stack<AABB::BoxType> : EnumWrapper<AABB::BoxType>{};
    template<> struct Stack<AABB::CornerEnum> : EnumWrapper<AABB::CornerEnum>{};
    template<> struct Stack<OBB::BoxType> : EnumWrapper<OBB::BoxType>{};
    template<> struct Stack<OBB::CornerEnum> : EnumWrapper<OBB::CornerEnum>{};
    
    template<> struct Stack<RayFilter> : EnumWrapper<RayFilter>{};
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

// Gradient:setKeys(colorKeys, alphaKeys): each colour key a GradientColorKey or {time, colour}
// (Vector3 / Vector4, sRGB), each alpha key a GradientAlphaKey or {time, alpha}; the lists
// replace the gradient's (sorted, at most 8 each). Colours are sRGB as the pickers show them,
// stored linear.
static int luaGradientSetKeys(lua_State* L){
    auto self = luabridge::get<Gradient*>(L, 1);
    if (!self || !*self){
        return luaL_error(L, "Gradient.setKeys: call it on a Gradient (gradient:setKeys(colorKeys, alphaKeys))");
    }
    if (!lua_istable(L, 2) || (!lua_isnoneornil(L, 3) && !lua_istable(L, 3))){
        return luaL_error(L, "Gradient.setKeys: expected (colorKeys, alphaKeys): tables of {time, color} and {time, alpha}");
    }
    std::vector<GradientColorKey> colors;
    const lua_Integer colorCount = (lua_Integer)lua_rawlen(L, 2);
    for (lua_Integer i = 1; i <= colorCount; i++){
        lua_rawgeti(L, 2, i);
        GradientColorKey key;
        bool ok = false;
        if (luabridge::isInstance<GradientColorKey>(L, -1)){
            auto value = luabridge::get<GradientColorKey>(L, -1);
            if (value){
                key = *value;
                ok = true;
            }
        }else if (lua_istable(L, -1)){
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            if (lua_isnumber(L, -2)){
                Vector3 srgb;
                if (luabridge::isInstance<Vector3>(L, -1)){
                    auto value = luabridge::get<Vector3>(L, -1);
                    if (value){ srgb = *value; ok = true; }
                }else if (luabridge::isInstance<Vector4>(L, -1)){
                    auto value = luabridge::get<Vector4>(L, -1);
                    if (value){ const Vector4 v = *value; srgb = Vector3(v.x, v.y, v.z); ok = true; }
                }
                key = GradientColorKey((float)lua_tonumber(L, -2), Color::sRGBToLinear(srgb));
            }
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        if (!ok){
            return luaL_error(L, "Gradient.setKeys: colorKeys[%d] must be {time, Vector3 color} or a GradientColorKey", (int)i);
        }
        colors.push_back(key);
    }
    std::vector<GradientAlphaKey> alphas;
    if (lua_istable(L, 3)){
        const lua_Integer alphaCount = (lua_Integer)lua_rawlen(L, 3);
        for (lua_Integer i = 1; i <= alphaCount; i++){
            lua_rawgeti(L, 3, i);
            GradientAlphaKey key;
            bool ok = false;
            if (luabridge::isInstance<GradientAlphaKey>(L, -1)){
                auto value = luabridge::get<GradientAlphaKey>(L, -1);
                if (value){
                    key = *value;
                    ok = true;
                }
            }else if (lua_istable(L, -1)){
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                if (lua_isnumber(L, -2) && lua_isnumber(L, -1)){
                    key = GradientAlphaKey((float)lua_tonumber(L, -2), (float)lua_tonumber(L, -1));
                    ok = true;
                }
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
            if (!ok){
                return luaL_error(L, "Gradient.setKeys: alphaKeys[%d] must be {time, alpha} or a GradientAlphaKey", (int)i);
            }
            alphas.push_back(key);
        }
    }
    if (colors.size() > Gradient::MAX_KEYS || alphas.size() > Gradient::MAX_KEYS){
        return luaL_error(L, "Gradient.setKeys: at most %d colour keys and %d alpha keys (got %d and %d)",
                          (int)Gradient::MAX_KEYS, (int)Gradient::MAX_KEYS, (int)colors.size(), (int)alphas.size());
    }
    (*self)->colorKeys = colors;
    (*self)->alphaKeys = alphas;
    (*self)->normalize();
    return 0;
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
        .addFunction("isNaN", &Vector4::isNaN)
        .endClass();

    // Unity AnimationCurve (lines and trails spec 6.1): keys are 1-based here (Lua), a
    // property of this type on a component is a copy: edit it, then assign it back
    luabridge::getGlobalNamespace(L)
        .beginNamespace("CurveWrap")
        .addVariable("Clamp", CurveWrap::Clamp)
        .addVariable("Loop", CurveWrap::Loop)
        .addVariable("PingPong", CurveWrap::PingPong)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<CurveKey>("CurveKey")
        .addConstructor<void(), void(float, float), void(float, float, float, float)>()
        .addProperty("time", &CurveKey::time, &CurveKey::time)
        .addProperty("value", &CurveKey::value, &CurveKey::value)
        // slopes (value per time unit); math.huge is a step
        .addProperty("inTangent", &CurveKey::inTangent, &CurveKey::inTangent)
        .addProperty("outTangent", &CurveKey::outTangent, &CurveKey::outTangent)
        .addFunction("__eq", &equalsLua<CurveKey>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimationCurve>("AnimationCurve")
        .addConstructor<void()>()
        .addStaticFunction("constant", &AnimationCurve::constant)
        .addStaticFunction("linear", &AnimationCurve::linear)
        .addStaticFunction("easeInOut", &AnimationCurve::easeInOut)
        .addFunction("evaluate", &AnimationCurve::evaluate)
        // the key's index (1-based), -1 when a key is at that time already
        .addFunction("addKey",
            luabridge::overload<AnimationCurve*, float, float>(+[](AnimationCurve* self, float time, float value) -> int {
                const int index = self->addKey(CurveKey(time, value));
                return index < 0 ? -1 : index + 1;
            }),
            luabridge::overload<AnimationCurve*, float, float, float, float>(+[](AnimationCurve* self, float time, float value, float inTangent, float outTangent) -> int {
                const int index = self->addKey(CurveKey(time, value, inTangent, outTangent));
                return index < 0 ? -1 : index + 1;
            }),
            luabridge::overload<AnimationCurve*, const CurveKey&>(+[](AnimationCurve* self, const CurveKey& key) -> int {
                const int index = self->addKey(key);
                return index < 0 ? -1 : index + 1;
            }))
        // false out of range (1..keyCount)
        .addFunction("removeKey", +[](AnimationCurve* self, int index) -> bool {
            return index >= 1 && self->removeKey((size_t)(index - 1));
        })
        // moves key `index` (its tangents kept, or the key given): the index it ends at, -1
        // when out of range or another key is at that time
        .addFunction("moveKey",
            luabridge::overload<AnimationCurve*, int, float, float>(+[](AnimationCurve* self, int index, float time, float value) -> int {
                if (index < 1 || (size_t)index > self->keys.size()) return -1;
                CurveKey key = self->keys[(size_t)(index - 1)];
                key.time = time;
                key.value = value;
                const int moved = self->moveKey((size_t)(index - 1), key);
                return moved < 0 ? -1 : moved + 1;
            }),
            luabridge::overload<AnimationCurve*, int, const CurveKey&>(+[](AnimationCurve* self, int index, const CurveKey& key) -> int {
                if (index < 1) return -1;
                const int moved = self->moveKey((size_t)(index - 1), key);
                return moved < 0 ? -1 : moved + 1;
            }))
        // a copy of key `index` (1-based), nil out of range
        .addFunction("getKey", +[](const AnimationCurve* self, int index) -> std::optional<CurveKey> {
            if (index < 1 || (size_t)index > self->keys.size()) return std::nullopt;
            return self->keys[(size_t)(index - 1)];
        })
        .addProperty("keyCount", +[](const AnimationCurve* self) -> int { return (int)self->keys.size(); })
        // every key (a copy); set it to replace them (sorted, equal times dropped)
        .addProperty("keys",
            +[](const AnimationCurve* self) -> std::vector<CurveKey> { return self->keys; },
            +[](AnimationCurve* self, std::vector<CurveKey> keys) { self->keys = std::move(keys); self->normalize(); })
        .addProperty("preWrapMode",
            +[](const AnimationCurve* self) -> CurveWrap { return self->preWrap; },
            +[](AnimationCurve* self, CurveWrap wrap) { self->preWrap = wrap; })
        .addProperty("postWrapMode",
            +[](const AnimationCurve* self) -> CurveWrap { return self->postWrap; },
            +[](AnimationCurve* self, CurveWrap wrap) { self->postWrap = wrap; })
        .addFunction("__eq", &equalsLua<AnimationCurve>)
        .endClass();

    // Unity Gradient: colours given and read in sRGB (as the pickers show them), stored
    // linear; evaluate gives linear RGBA. Keys are 1-based; at most 8 of each
    luabridge::getGlobalNamespace(L)
        .beginNamespace("GradientMode")
        .addVariable("Blend", GradientMode::Blend)
        .addVariable("Fixed", GradientMode::Fixed)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<GradientColorKey>("GradientColorKey")
        .addConstructor(
            +[](void* ptr) { return new (ptr) GradientColorKey(); },
            +[](void* ptr, float time, const Vector3& color) { return new (ptr) GradientColorKey(time, Color::sRGBToLinear(color)); })
        .addProperty("time", &GradientColorKey::time, &GradientColorKey::time)
        // sRGB
        .addProperty("color",
            +[](const GradientColorKey* self) -> Vector3 { return Color::linearTosRGB(self->color); },
            +[](GradientColorKey* self, const Vector3& color) { self->color = Color::sRGBToLinear(color); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<GradientAlphaKey>("GradientAlphaKey")
        .addConstructor<void(), void(float, float)>()
        .addProperty("time", &GradientAlphaKey::time, &GradientAlphaKey::time)
        .addProperty("alpha", &GradientAlphaKey::alpha, &GradientAlphaKey::alpha)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Gradient>("Gradient")
        // white, opaque (a key at each end)
        .addConstructor<void()>()
        .addFunction("setKeys", &luaGradientSetKeys)
        // linear RGBA at t (0..1)
        .addFunction("evaluate", &Gradient::evaluate)
        .addProperty("mode",
            +[](const Gradient* self) -> GradientMode { return self->mode; },
            +[](Gradient* self, GradientMode mode) { self->mode = mode; })
        .addProperty("colorKeyCount", +[](const Gradient* self) -> int { return (int)self->colorKeys.size(); })
        .addProperty("alphaKeyCount", +[](const Gradient* self) -> int { return (int)self->alphaKeys.size(); })
        // a copy of colour key `index` (1-based; its colour sRGB), nil out of range
        .addFunction("getColorKey", +[](const Gradient* self, int index) -> std::optional<GradientColorKey> {
            if (index < 1 || (size_t)index > self->colorKeys.size()) return std::nullopt;
            return self->colorKeys[(size_t)(index - 1)];
        })
        .addFunction("getAlphaKey", +[](const Gradient* self, int index) -> std::optional<GradientAlphaKey> {
            if (index < 1 || (size_t)index > self->alphaKeys.size()) return std::nullopt;
            return self->alphaKeys[(size_t)(index - 1)];
        })
        .addProperty("colorKeys",
            +[](const Gradient* self) -> std::vector<GradientColorKey> { return self->colorKeys; },
            +[](Gradient* self, std::vector<GradientColorKey> keys, lua_State* L) {
                if (keys.size() > Gradient::MAX_KEYS){
                    luaL_error(L, "Gradient.colorKeys: at most %d keys (got %d)", (int)Gradient::MAX_KEYS, (int)keys.size());
                    return;
                }
                self->colorKeys = std::move(keys);
                self->normalize();
            })
        .addProperty("alphaKeys",
            +[](const Gradient* self) -> std::vector<GradientAlphaKey> { return self->alphaKeys; },
            +[](Gradient* self, std::vector<GradientAlphaKey> keys, lua_State* L) {
                if (keys.size() > Gradient::MAX_KEYS){
                    luaL_error(L, "Gradient.alphaKeys: at most %d keys (got %d)", (int)Gradient::MAX_KEYS, (int)keys.size());
                    return;
                }
                self->alphaKeys = std::move(keys);
                self->normalize();
            })
        .addFunction("__eq", &equalsLua<Gradient>)
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
        .addFunction("getCorners", &AABB::getCorners)
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
        .addFunction("getCorners", &OBB::getCorners)
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
