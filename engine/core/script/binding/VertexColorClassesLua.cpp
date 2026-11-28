// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// V1 R80-R83, R125: Lua's VertexColors (object/VertexColors.h), VertexColors(scene, entity). A target
// is its index in `targets` (1-based) or a table {node = n, primitive = p} / {submesh = s}; vertex
// ids are 0-based GPU vertices, as in the op tables. applyOps reads edit_mesh_colors' op tables.
// Lua is compiled as C and luaL_error longjmps past C++ destructors: the functions that build C++
// objects do their work in a helper that returns, and raise only after it has.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "object/VertexColors.h"
#include "picking/MeshRaycast.h"
#include "script/LuaJson.h"
#include "vertexcolor/VertexColorOpsJson.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    VertexColors* vertexColorsSelf(lua_State* L, const char* function){
        auto self = luabridge::get<VertexColors*>(L, 1);
        if (!self || !*self){
            lua_pushfstring(L, "VertexColors:%s: call it on a VertexColors with ':'", function);
            return nullptr;
        }
        return *self;
    }

    bool vertexColorsFail(lua_State* L, const char* function, const std::string& error){
        lua_pushstring(L, (std::string("VertexColors:") + function + ": " + error).c_str());
        return false;
    }

    // a target argument: an index into `targets` (1-based) or {node =, primitive =} / {submesh =}
    bool vertexColorsTarget(lua_State* L, int index, VertexColors* self, vertexcolor::TargetKey& key, std::string& error){
        if (lua_isinteger(L, index)){
            const lua_Integer n = lua_tointeger(L, index);
            const std::vector<vertexcolor::SceneTarget> targets = self->getTargets();
            if (n < 1 || n > (lua_Integer)targets.size()){
                error = "target " + std::to_string((long long)n) + " is not one of the " + std::to_string(targets.size()) + " targets (1-based)";
                return false;
            }
            key = targets[(size_t)(n - 1)].key;
            return true;
        }
        if (lua_istable(L, index)){
            auto field = [&](const char* name, int32_t& out){
                lua_getfield(L, index, name);
                const bool present = lua_isinteger(L, -1);
                if (present)
                    out = (int32_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
                return present;
            };
            key = vertexcolor::TargetKey();
            const bool node = field("node", key.node);
            const bool primitive = field("primitive", key.primitive);
            const bool submesh = field("submesh", key.submesh);
            if ((node && primitive && !submesh) || (submesh && !node && !primitive))
                return true;
        }
        error = "the target must be an index into targets (1-based), {node = n, primitive = p} or {submesh = s}";
        return false;
    }

    bool vertexColorsVector(lua_State* L, int index, Vector4& out){
        if (luabridge::isInstance<Vector4>(L, index)){
            auto value = luabridge::get<Vector4*>(L, index);
            if (!value || !*value)
                return false;
            out = **value;
            return true;
        }
        if (!lua_istable(L, index))
            return false;
        const lua_Integer n = (lua_Integer)lua_rawlen(L, index);
        if (n != 3 && n != 4)
            return false;
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (lua_Integer i = 1; i <= n; i++){
            lua_rawgeti(L, index, i);
            const bool number = lua_type(L, -1) == LUA_TNUMBER;
            v[i - 1] = number ? (float)lua_tonumber(L, -1) : 0.0f;
            lua_pop(L, 1);
            if (!number || !std::isfinite(v[i - 1]))
                return false;
        }
        out = Vector4(v[0], v[1], v[2], v[3]);
        return true;
    }

    const MeshRaycastHit* vertexColorsHit(lua_State* L, int index){
        if (!luabridge::isInstance<MeshRaycastHit>(L, index))
            return nullptr;
        auto hit = luabridge::get<MeshRaycastHit*>(L, index);
        return (hit && *hit) ? *hit : nullptr;
    }

    const Vector3* vertexColorsPoint(lua_State* L, int index){
        if (!luabridge::isInstance<Vector3>(L, index))
            return nullptr;
        auto point = luabridge::get<Vector3*>(L, index);
        return (point && *point) ? *point : nullptr;
    }

    void vertexColorsPushVector(lua_State* L, const Vector4& value){
        if (!luabridge::push(L, value))
            lua_pushnil(L);
    }

    // vc:applyOps(ops [, target])
    bool vertexColorsApplyOps(lua_State* L){
        VertexColors* self = vertexColorsSelf(L, "applyOps");
        if (!self)
            return false;
        if (!lua_istable(L, 2))
            return vertexColorsFail(L, "applyOps", "ops must be a table of op tables");
        nlohmann::json list;
        std::string error;
        if (!LuaJson::toJson(L, 2, list, LuaJson::EncodeOptions(), error))
            return vertexColorsFail(L, "applyOps", error);
        vertexcolor::TargetKey key;
        const bool one = !lua_isnoneornil(L, 3);
        if (one && !vertexColorsTarget(L, 3, self, key, error))
            return vertexColorsFail(L, "applyOps", error);
        nlohmann::json result;
        if (!self->applyOps(list, one ? &key : nullptr, result, error))
            return vertexColorsFail(L, "applyOps", error);
        LuaJson::push(L, result);
        return true;
    }

    // vc:getValue(target, vertex) / vc:getValues(target, first, count)
    bool vertexColorsGetValues(lua_State* L, bool single){
        const char* function = single ? "getValue" : "getValues";
        VertexColors* self = vertexColorsSelf(L, function);
        if (!self)
            return false;
        vertexcolor::TargetKey key;
        std::string error;
        if (!vertexColorsTarget(L, 2, self, key, error))
            return vertexColorsFail(L, function, error);
        if (!lua_isinteger(L, 3) || lua_tointeger(L, 3) < 0)
            return vertexColorsFail(L, function, "the vertex must be an integer >= 0 (0-based)");
        const uint32_t first = (uint32_t)lua_tointeger(L, 3);
        uint32_t count = 1;
        if (!single){
            if (!lua_isinteger(L, 4) || lua_tointeger(L, 4) < 0)
                return vertexColorsFail(L, function, "count must be an integer >= 0");
            if (lua_tointeger(L, 4) > (lua_Integer)VertexColors::MAX_VALUES)
                return vertexColorsFail(L, function, "count " + std::to_string((long long)lua_tointeger(L, 4)) + " is over the cap of " +
                                                     std::to_string(VertexColors::MAX_VALUES) + " values: read in pages");
            count = (uint32_t)lua_tointeger(L, 4);
        }
        std::vector<Vector4> values;
        if (!self->getValues(key, first, count, values, error))
            return vertexColorsFail(L, function, error);
        if (single){
            vertexColorsPushVector(L, values[0]);
            return true;
        }
        lua_createtable(L, (int)values.size(), 0);
        for (size_t i = 0; i < values.size(); i++){
            vertexColorsPushVector(L, values[i]);
            lua_rawseti(L, -2, (int)(i + 1));
        }
        return true;
    }

    // vc:setValues(target, first, values)
    bool vertexColorsSetValues(lua_State* L){
        VertexColors* self = vertexColorsSelf(L, "setValues");
        if (!self)
            return false;
        vertexcolor::TargetKey key;
        std::string error;
        if (!vertexColorsTarget(L, 2, self, key, error))
            return vertexColorsFail(L, "setValues", error);
        if (!lua_isinteger(L, 3) || lua_tointeger(L, 3) < 0)
            return vertexColorsFail(L, "setValues", "first must be an integer >= 0 (0-based)");
        if (!lua_istable(L, 4))
            return vertexColorsFail(L, "setValues", "values must be a table of Vector4 (or {r, g, b[, a]})");
        const uint32_t first = (uint32_t)lua_tointeger(L, 3);
        const lua_Integer n = (lua_Integer)lua_rawlen(L, 4);
        if (n > (lua_Integer)VertexColors::MAX_VALUES)
            return vertexColorsFail(L, "setValues", "values has " + std::to_string((long long)n) + " entries, over the cap of " +
                                                    std::to_string(VertexColors::MAX_VALUES) + ": write in pages");
        std::vector<Vector4> values((size_t)n);
        for (lua_Integer i = 1; i <= n; i++){
            lua_rawgeti(L, 4, i);
            const bool ok = vertexColorsVector(L, lua_gettop(L), values[(size_t)(i - 1)]);
            lua_pop(L, 1);
            if (!ok)
                return vertexColorsFail(L, "setValues", "values[" + std::to_string((long long)i) + "] is not a Vector4 or {r, g, b[, a]} of numbers");
        }
        if (!self->setValues(key, first, values, error))
            return vertexColorsFail(L, "setValues", error);
        lua_pushboolean(L, 1);
        return true;
    }

    // vc:stats(target)
    bool vertexColorsStats(lua_State* L){
        VertexColors* self = vertexColorsSelf(L, "stats");
        if (!self)
            return false;
        vertexcolor::TargetKey key;
        std::string error;
        if (!vertexColorsTarget(L, 2, self, key, error))
            return vertexColorsFail(L, "stats", error);
        nlohmann::json stats;
        if (!self->getStats(key, stats, error))
            return vertexColorsFail(L, "stats", error);
        LuaJson::push(L, stats);
        return true;
    }

    // vc:sampleAt(worldPoint | hit): the Vector4 drawn there, nil when nothing is
    bool vertexColorsSampleAt(lua_State* L){
        VertexColors* self = vertexColorsSelf(L, "sampleAt");
        if (!self)
            return false;
        Vector4 value;
        std::string error;
        bool found = false;
        const MeshRaycastHit* hit = vertexColorsHit(L, 2);
        const Vector3* point = vertexColorsPoint(L, 2);
        if (hit){
            found = self->sampleAt(*hit, value, error);
        }else if (point){
            found = self->sampleAt(*point, value, error);
        }else{
            return vertexColorsFail(L, "sampleAt", "pass a world point (Vector3) or a MeshRaycast hit");
        }
        if (!found && !self->isValid())
            return vertexColorsFail(L, "sampleAt", error);
        if (found)
            vertexColorsPushVector(L, value);
        else
            lua_pushnil(L);
        return true;
    }

    // vc:paint(worldPoint | hit, {radius, value, channels, strength, hardness, hold, fade})
    bool vertexColorsPaint(lua_State* L){
        VertexColors* self = vertexColorsSelf(L, "paint");
        if (!self)
            return false;
        vertexcolor::RuntimePaint paint;
        std::string error;
        if (lua_istable(L, 3)){
            nlohmann::json options;
            if (!LuaJson::toJson(L, 3, options, LuaJson::EncodeOptions(), error))
                return vertexColorsFail(L, "paint", error);
            auto number = [&](const char* name, float& out){
                auto it = options.find(name);
                if (it == options.end())
                    return true;
                if (!it->is_number() || !std::isfinite(it->get<double>())){
                    error = std::string("opts.") + name + " must be a number";
                    return false;
                }
                out = it->get<float>();
                return true;
            };
            if (!number("radius", paint.radius) || !number("strength", paint.strength) || !number("hardness", paint.hardness) ||
                    !number("hold", paint.hold) || !number("fade", paint.fade))
                return vertexColorsFail(L, "paint", error);
            bool rgb = false;
            auto value = options.find("value");
            if (value != options.end()){
                nlohmann::json v = *value;
                // a Vector4 arrives as {x, y, z, w}
                if (v.is_object() && v.contains("x"))
                    v = nlohmann::json::array({v.value("x", 0.0), v.value("y", 0.0), v.value("z", 0.0), v.value("w", 1.0)});
                rgb = v.is_array() && v.size() == 3;
                if (!vertexcolor::parseValue(v, paint.value, error))
                    return vertexColorsFail(L, "paint", "opts.value: " + error);
            }
            auto channels = options.find("channels");
            if (channels != options.end()){
                if (!vertexcolor::parseChannels(*channels, paint.channels, error))
                    return vertexColorsFail(L, "paint", "opts.channels: " + error);
            }else{
                paint.channels = rgb ? vertexcolor::CHANNELS_RGB : vertexcolor::CHANNELS_RGBA;
            }
        }else if (!lua_isnoneornil(L, 3)){
            return vertexColorsFail(L, "paint", "opts must be a table");
        }
        uint32_t touched = 0;
        const MeshRaycastHit* hit = vertexColorsHit(L, 2);
        const Vector3* point = vertexColorsPoint(L, 2);
        if (hit){
            touched = self->paint(*hit, paint, error);
        }else if (point){
            touched = self->paint(*point, paint, error);
        }else{
            return vertexColorsFail(L, "paint", "pass a world point (Vector3) or a MeshRaycast hit");
        }
        if (!error.empty())
            return vertexColorsFail(L, "paint", error);
        lua_pushinteger(L, (lua_Integer)touched);
        return true;
    }

    luabridge::LuaRef vertexColorsChannelNames(const VertexColors* self, lua_State* L){
        luabridge::LuaRef table = luabridge::newTable(L);
        const std::vector<std::string> names = self->getChannelNames();
        for (size_t i = 0; i < names.size(); i++)
            table[(int)(i + 1)] = names[i];
        return table;
    }

    luabridge::LuaRef vertexColorsTargets(const VertexColors* self, lua_State* L){
        luabridge::LuaRef list = luabridge::newTable(L);
        int i = 1;
        for (const vertexcolor::SceneTarget& target : self->getTargets()){
            luabridge::LuaRef entry = luabridge::newTable(L);
            if (target.key.submesh >= 0){
                entry["submesh"] = target.key.submesh;
            }else{
                entry["node"] = target.key.node;
                entry["primitive"] = target.key.primitive;
            }
            entry["name"] = target.name;
            entry["vertexCount"] = target.vertexCount;
            entry["triangleCount"] = target.triangleCount;
            list[i++] = entry;
        }
        return list;
    }

}

void LuaBinding::registerVertexColorClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .deriveClass<VertexColors, EntityHandle>("VertexColors")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &VertexColors::isValid)
        .addProperty("enabled", &VertexColors::isEnabled, &VertexColors::setEnabled)
        .addProperty("format", &VertexColors::getFormat)
        .addProperty("channelNames", &vertexColorsChannelNames)
        .addProperty("targets", &vertexColorsTargets)
        .addFunction("applyOps", +[](lua_State* L) -> int { return vertexColorsApplyOps(L) ? 1 : lua_error(L); })
        .addFunction("getValue", +[](lua_State* L) -> int { return vertexColorsGetValues(L, true) ? 1 : lua_error(L); })
        .addFunction("getValues", +[](lua_State* L) -> int { return vertexColorsGetValues(L, false) ? 1 : lua_error(L); })
        .addFunction("setValues", +[](lua_State* L) -> int { return vertexColorsSetValues(L) ? 1 : lua_error(L); })
        .addFunction("stats", +[](lua_State* L) -> int { return vertexColorsStats(L) ? 1 : lua_error(L); })
        .addFunction("sampleAt", +[](lua_State* L) -> int { return vertexColorsSampleAt(L) ? 1 : lua_error(L); })
        .addFunction("paint", +[](lua_State* L) -> int { return vertexColorsPaint(L) ? 1 : lua_error(L); })
        .addFunction("clearRuntime", &VertexColors::clearRuntime)
        .endClass();
}
