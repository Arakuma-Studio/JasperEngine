// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptProperty.h"
#include "Log.h"
#include "LuaBinding.h"
#include "lua.hpp"
#include "LuaBridge.h"
#include "asset/AssetMap.h"
#include "script/DataAsset.h"
#include "Scene.h"

#include <algorithm>
#include <cctype>

namespace jasperengine {

    ScriptPropertyValue scriptPropertyValueOf(const ScriptPropertyElement& element) {
        return std::visit([](const auto& value) -> ScriptPropertyValue { return value; }, element);
    }

    ScriptPropertyElement scriptPropertyElementOf(const ScriptPropertyValue& value) {
        return std::visit([](const auto& held) -> ScriptPropertyElement {
            using V = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<V, ScriptPropertyList>) return std::monostate{};
            else return held;
        }, value);
    }

    ScriptPropertyElement scriptPropertyElementDefault(ScriptPropertyType type) {
        switch (type) {
            case ScriptPropertyType::Bool: return false;
            case ScriptPropertyType::Int: return 0;
            case ScriptPropertyType::Float: return 0.0f;
            case ScriptPropertyType::String: return std::string();
            case ScriptPropertyType::Vector2: return Vector2(0, 0);
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: return Vector3(0, 0, 0);
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: return Vector4(0, 0, 0, 1);
            case ScriptPropertyType::EntityReference: return EntityReference{};
            case ScriptPropertyType::Prefab: return PrefabAsset();
            case ScriptPropertyType::Asset: return AssetRef();
            case ScriptPropertyType::List: break;
        }
        return std::monostate{};
    }

    bool scriptPropertyTypeFromName(const std::string& name, ScriptPropertyType& type) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::pair<const char*, ScriptPropertyType> names[] = {
            {"bool", ScriptPropertyType::Bool}, {"boolean", ScriptPropertyType::Bool},
            {"int", ScriptPropertyType::Int}, {"integer", ScriptPropertyType::Int},
            {"float", ScriptPropertyType::Float}, {"number", ScriptPropertyType::Float},
            {"string", ScriptPropertyType::String},
            {"vector2", ScriptPropertyType::Vector2}, {"vec2", ScriptPropertyType::Vector2},
            {"vector3", ScriptPropertyType::Vector3}, {"vec3", ScriptPropertyType::Vector3},
            {"vector4", ScriptPropertyType::Vector4}, {"vec4", ScriptPropertyType::Vector4},
            {"color3", ScriptPropertyType::Color3}, {"color4", ScriptPropertyType::Color4},
            {"prefab", ScriptPropertyType::Prefab}, {"prefabasset", ScriptPropertyType::Prefab},
            {"asset", ScriptPropertyType::Asset}, {"assetref", ScriptPropertyType::Asset},
            {"list", ScriptPropertyType::List},
            {"entity", ScriptPropertyType::EntityReference}, {"entitypointer", ScriptPropertyType::EntityReference},
            {"pointer", ScriptPropertyType::EntityReference},
        };
        for (const auto& [text, value] : names) {
            if (lower == text) {
                type = value;
                return true;
            }
        }
        return false;
    }

    bool ScriptProperty::isDataAssetReference() const {
        const bool asset = type == ScriptPropertyType::Asset || (type == ScriptPropertyType::List && elementType == ScriptPropertyType::Asset);
        return asset && assetType == DataAsset::ASSET_TYPE;
    }

    namespace {

        // a list element as it was set from Lua: numbers, strings, vectors, PrefabAsset, AssetRef
        // or a Data Asset's table; false when the value does not convert (an entity is never read
        // back: the stored reference stays)
        bool readLuaElement(lua_State* L, int index, const ScriptProperty& prop, ScriptPropertyElement& out) {
            switch (prop.elementType) {
                case ScriptPropertyType::Bool:
                    if (!lua_isboolean(L, index)) return false;
                    out = lua_toboolean(L, index) != 0;
                    return true;
                case ScriptPropertyType::Int:
                    if (!lua_isnumber(L, index)) return false;
                    out = lua_isinteger(L, index) ? static_cast<int>(lua_tointeger(L, index)) : static_cast<int>(lua_tonumber(L, index));
                    return true;
                case ScriptPropertyType::Float:
                    if (!lua_isnumber(L, index)) return false;
                    out = static_cast<float>(lua_tonumber(L, index));
                    return true;
                case ScriptPropertyType::String:
                    if (lua_type(L, index) != LUA_TSTRING) return false;
                    out = std::string(lua_tostring(L, index));
                    return true;
                case ScriptPropertyType::Vector2: {
                    auto result = luabridge::Stack<Vector2>::get(L, index);
                    if (!result) return false;
                    out = result.value();
                    return true;
                }
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3: {
                    auto result = luabridge::Stack<Vector3>::get(L, index);
                    if (!result) return false;
                    out = result.value();
                    return true;
                }
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4: {
                    auto result = luabridge::Stack<Vector4>::get(L, index);
                    if (!result) return false;
                    out = result.value();
                    return true;
                }
                case ScriptPropertyType::Prefab: {
                    if (lua_isnil(L, index) || (lua_isboolean(L, index) && !lua_toboolean(L, index))) {
                        out = PrefabAsset();
                        return true;
                    }
                    auto result = luabridge::Stack<PrefabAsset>::get(L, index);
                    if (!result) return false;
                    out = result.value();
                    return true;
                }
                case ScriptPropertyType::Asset: {
                    if (lua_isnil(L, index) || (lua_isboolean(L, index) && !lua_toboolean(L, index))) {
                        out = AssetRef();
                        return true;
                    }
                    if (prop.assetType == DataAsset::ASSET_TYPE) {
                        std::string path;
                        if (!DataAsset::pathOf(L, index, path)) return false;
                        out = AssetRef(path, AssetMap::findGuid(path));
                        return true;
                    }
                    if (lua_type(L, index) == LUA_TSTRING) {
                        out = ScriptAssetRef::make(lua_tostring(L, index)).getRef();
                        return true;
                    }
                    auto result = luabridge::Stack<ScriptAssetRef>::get(L, index);
                    if (!result) return false;
                    out = result.value().getRef();
                    return true;
                }
                case ScriptPropertyType::EntityReference:
                case ScriptPropertyType::List:
                    break;
            }
            return false;
        }

        // the Scene of a Lua instance table at `index` (its `scene` field), null when it has none
        Scene* instanceScene(lua_State* L, int index) {
            lua_getfield(L, index, "scene");
            auto result = luabridge::Stack<Scene*>::get(L, -1);
            lua_pop(L, 1);
            return result ? result.value() : nullptr;
        }

    }

    void ScriptProperty::reportStaleValue() const {
        Log::warn("Script property '%s' had a value of another type, reset to default", name.c_str());
    }

    void ScriptProperty::syncToMember() {
        if (memberPtr) {
            switch (type) {
                case ScriptPropertyType::Bool:
                    if (std::holds_alternative<bool>(value)) {
                        *static_cast<bool*>(memberPtr) = std::get<bool>(value);
                    }
                    break;
                case ScriptPropertyType::Int:
                    if (std::holds_alternative<int>(value)) {
                        *static_cast<int*>(memberPtr) = std::get<int>(value);
                    }
                    break;
                case ScriptPropertyType::Float:
                    if (std::holds_alternative<float>(value)) {
                        *static_cast<float*>(memberPtr) = std::get<float>(value);
                    }
                    break;
                case ScriptPropertyType::String:
                    if (std::holds_alternative<std::string>(value)) {
                        *static_cast<std::string*>(memberPtr) = std::get<std::string>(value);
                    }
                    break;
                case ScriptPropertyType::Vector2:
                    if (std::holds_alternative<Vector2>(value)) {
                        *static_cast<Vector2*>(memberPtr) = std::get<Vector2>(value);
                    }
                    break;
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3:
                    if (std::holds_alternative<Vector3>(value)) {
                        *static_cast<Vector3*>(memberPtr) = std::get<Vector3>(value);
                    }
                    break;
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4:
                    if (std::holds_alternative<Vector4>(value)) {
                        *static_cast<Vector4*>(memberPtr) = std::get<Vector4>(value);
                    }
                    break;
                case ScriptPropertyType::EntityReference:
                    // Intentionally no-op for runtime member sync
                    break;
                case ScriptPropertyType::Prefab:
                    if (std::holds_alternative<PrefabAsset>(value)) {
                        *static_cast<PrefabAsset*>(memberPtr) = std::get<PrefabAsset>(value);
                    } else {
                        *static_cast<PrefabAsset*>(memberPtr) = PrefabAsset();
                    }
                    break;
                case ScriptPropertyType::Asset:
                    if (std::holds_alternative<AssetRef>(value)) {
                        *static_cast<AssetRef*>(memberPtr) = std::get<AssetRef>(value);
                    } else {
                        *static_cast<AssetRef*>(memberPtr) = AssetRef();
                    }
                    break;
                case ScriptPropertyType::List:
                    // a Lua property type: C++ scripts declare none
                    break;
            }
        } else if (luaRef != 0 && !name.empty()) {
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;

            lua_rawgeti(L, LUA_REGISTRYINDEX, luaRef); // Push instance table
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return;
            }

            // a list and a Data Asset are what the instance got at its start (their references resolved in its scene)
            if (type == ScriptPropertyType::List || isDataAssetReference()) {
                LuaBinding::pushScriptPropertyValue(L, instanceScene(L, -1), *this);
                lua_setfield(L, -2, name.c_str());
                lua_pop(L, 1);
                return;
            }

            auto pushOrNil = [&](auto&& v) {
                const int top = lua_gettop(L);
                auto r = luabridge::push(L, std::forward<decltype(v)>(v));
                if (!r) {
                    lua_settop(L, top); // rollback anything partially pushed
                    lua_pushnil(L);
                }
            };

            // Push the value based on type
            switch (type) {
                case ScriptPropertyType::Bool:
                    if (std::holds_alternative<bool>(value)) {
                        lua_pushboolean(L, std::get<bool>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Int:
                    if (std::holds_alternative<int>(value)) {
                        lua_pushinteger(L, std::get<int>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Float:
                    if (std::holds_alternative<float>(value)) {
                        lua_pushnumber(L, std::get<float>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::String:
                    if (std::holds_alternative<std::string>(value)) {
                        lua_pushstring(L, std::get<std::string>(value).c_str());
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Vector2:
                    if (std::holds_alternative<Vector2>(value)) {
                        pushOrNil(std::get<Vector2>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3:
                    if (std::holds_alternative<Vector3>(value)) {
                        pushOrNil(std::get<Vector3>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4:
                    if (std::holds_alternative<Vector4>(value)) {
                        pushOrNil(std::get<Vector4>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::EntityReference:
                    if (std::holds_alternative<EntityReference>(value)) {
                        lua_pushinteger(L, static_cast<lua_Integer>(std::get<EntityReference>(value).entity));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Prefab:
                    if (std::holds_alternative<PrefabAsset>(value) && !std::get<PrefabAsset>(value).isEmpty()) {
                        pushOrNil(std::get<PrefabAsset>(value));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::Asset:
                    if (std::holds_alternative<AssetRef>(value) && std::get<AssetRef>(value).isSet()) {
                        pushOrNil(ScriptAssetRef(std::get<AssetRef>(value)));
                    } else {
                        lua_pushnil(L);
                    }
                    break;
                case ScriptPropertyType::List:
                    lua_pushnil(L);
                    break;
            }

            lua_setfield(L, -2, name.c_str()); // instance[name] = value
            lua_pop(L, 1); // Pop instance table
        }
    }

    void ScriptProperty::syncFromMember() {
        if (memberPtr) {
            switch (type) {
                case ScriptPropertyType::Bool:
                    value = *static_cast<bool*>(memberPtr);
                    break;
                case ScriptPropertyType::Int:
                    value = *static_cast<int*>(memberPtr);
                    break;
                case ScriptPropertyType::Float:
                    value = *static_cast<float*>(memberPtr);
                    break;
                case ScriptPropertyType::String:
                    value = *static_cast<std::string*>(memberPtr);
                    break;
                case ScriptPropertyType::Vector2:
                    value = *static_cast<Vector2*>(memberPtr);
                    break;
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3:
                    value = *static_cast<Vector3*>(memberPtr);
                    break;
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4:
                    value = *static_cast<Vector4*>(memberPtr);
                    break;
                case ScriptPropertyType::EntityReference:
                    // Intentionally no-op for runtime member sync
                    break;
                case ScriptPropertyType::Prefab:
                    value = *static_cast<PrefabAsset*>(memberPtr);
                    break;
                case ScriptPropertyType::Asset:
                    value = *static_cast<AssetRef*>(memberPtr);
                    break;
                case ScriptPropertyType::List:
                    break;
            }
        } else if (luaRef != 0 && !name.empty()) {
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;

            lua_rawgeti(L, LUA_REGISTRYINDEX, luaRef); // Push instance table
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return;
            }

            lua_getfield(L, -1, name.c_str()); // Push instance[name]

            switch (type) {
                case ScriptPropertyType::Bool:
                    if (lua_isboolean(L, -1)) {
                        value = static_cast<bool>(lua_toboolean(L, -1));
                    }
                    break;
                case ScriptPropertyType::Int:
                    if (lua_isinteger(L, -1)) {
                        value = static_cast<int>(lua_tointeger(L, -1));
                    } else if (lua_isnumber(L, -1)) {
                        value = static_cast<int>(lua_tonumber(L, -1));
                    }
                    break;
                case ScriptPropertyType::Float:
                    if (lua_isnumber(L, -1)) {
                        value = static_cast<float>(lua_tonumber(L, -1));
                    }
                    break;
                case ScriptPropertyType::String:
                    if (lua_isstring(L, -1)) {
                        value = std::string(lua_tostring(L, -1));
                    }
                    break;
                case ScriptPropertyType::Vector2:
                    {
                        auto result = luabridge::Stack<Vector2>::get(L, -1);
                        if (result) {
                            value = result.value();
                        }
                    }
                    break;
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3:
                    {
                        auto result = luabridge::Stack<Vector3>::get(L, -1);
                        if (result) {
                            value = result.value();
                        }
                    }
                    break;
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4:
                    {
                        auto result = luabridge::Stack<Vector4>::get(L, -1);
                        if (result) {
                            value = result.value();
                        }
                    }
                    break;
                case ScriptPropertyType::Prefab: {
                    if (lua_isnil(L, -1)) {
                        value = PrefabAsset();
                    } else {
                        auto result = luabridge::Stack<PrefabAsset>::get(L, -1);
                        if (result) {
                            value = result.value();
                        }
                    }
                    break;
                }
                case ScriptPropertyType::List: {
                    // the elements that still convert (a table edited in Play); entity elements keep their reference
                    if (!lua_istable(L, -1) || elementType == ScriptPropertyType::EntityReference) break;
                    ScriptPropertyList list;
                    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
                    for (lua_Integer i = 1; i <= count; i++) {
                        lua_rawgeti(L, -1, i);
                        ScriptPropertyElement element = scriptPropertyElementDefault(elementType);
                        readLuaElement(L, -1, *this, element);
                        list.items.push_back(std::move(element));
                        lua_pop(L, 1);
                    }
                    value = std::move(list);
                    break;
                }
                case ScriptPropertyType::Asset: {
                    // nil, an AssetRef, or a GUID / path string; a Data Asset's table
                    if (assetType == DataAsset::ASSET_TYPE) {
                        ScriptPropertyElement element = AssetRef();
                        ScriptProperty single = *this;
                        single.elementType = ScriptPropertyType::Asset;
                        if (readLuaElement(L, -1, single, element)) value = std::get<AssetRef>(element);
                    } else if (lua_isnil(L, -1)) {
                        value = AssetRef();
                    } else if (lua_type(L, -1) == LUA_TSTRING) {
                        value = ScriptAssetRef::make(lua_tostring(L, -1)).getRef();
                    } else {
                        auto result = luabridge::Stack<ScriptAssetRef>::get(L, -1);
                        if (result) {
                            value = result.value().getRef();
                        }
                    }
                    break;
                }
                case ScriptPropertyType::EntityReference: {
                    uint32_t sid = std::holds_alternative<EntityReference>(value) ? std::get<EntityReference>(value).sceneId : 0;
                    if (lua_isinteger(L, -1)) {
                        value = EntityReference{static_cast<Entity>(lua_tointeger(L, -1)), sid};
                    } else if (lua_isnumber(L, -1)) {
                        value = EntityReference{static_cast<Entity>(lua_tonumber(L, -1)), sid};
                    }
                    break;
                }
            }

            lua_pop(L, 2); // Pop value and instance table
        }
    }

}