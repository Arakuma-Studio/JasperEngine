// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R75: Lua on Terrain. Queries take world XZ and are always available; writes need the
// terrain's Allow Runtime Edits (R77), detach its maps from the authored files first (R78) and
// have no undo. applyOps reads the op tables edit_terrain reads (object/TerrainOpsJson.h).
// Lua is compiled as C and luaL_error longjmps past C++ destructors: the functions that build C++
// objects do their work in a helper that returns, and raise only after it has.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "object/Terrain.h"
#include "object/TerrainEdit.h"
#include "script/LuaJson.h"
#include "terrain/TerrainOps.h"
#include "object/TerrainOpsJson.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    using terrain::MapId;
    using terrain::MapKind;
    using terrain::TexelRect;

    Terrain* terrainLuaSelf(lua_State* L) {
        auto self = luabridge::get<Terrain*>(L, 1);
        if (!self || !*self) luaL_argerror(L, 1, "a Terrain (call it with ':')");
        return *self;
    }

    // World XZ to the terrain's local XZ, on its plane
    Vector2 terrainLuaLocal(const Transform& transform, float x, float z) {
        const Vector3 local = transform.modelMatrix.inverse() * Vector3(x, transform.worldPosition.y, z);
        return Vector2(local.x, local.z);
    }

    Vector3 terrainLuaWorldNormal(const Transform& transform, const Vector3& normal) {
        const Vector4 n = transform.modelMatrix.inverse().transpose() * Vector4(normal.x, normal.y, normal.z, 0.0f);
        return Vector3(n.x, n.y, n.z).normalized();
    }

    float terrainLuaHeightAt(Terrain* self, float x, float z) {
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        const Transform& transform = self->getComponent<Transform>();
        const Vector2 local = terrainLuaLocal(transform, x, z);
        const terrain::SurfaceSample sample = querySurface(terrain, local.x, local.y);
        return (transform.modelMatrix * Vector3(local.x, sample.height, local.y)).y;
    }

    Vector3 terrainLuaNormalAt(Terrain* self, float x, float z) {
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        const Transform& transform = self->getComponent<Transform>();
        const Vector2 local = terrainLuaLocal(transform, x, z);
        return terrainLuaWorldNormal(transform, querySurface(terrain, local.x, local.y).normal);
    }

    bool terrainLuaIsHoleAt(Terrain* self, float x, float z) {
        const Vector2 local = terrainLuaLocal(self->getComponent<Transform>(), x, z);
        return querySurface(self->getComponent<TerrainComponent>(), local.x, local.y).hole;
    }

    bool terrainLuaContains(Terrain* self, float x, float z) {
        const Vector2 local = terrainLuaLocal(self->getComponent<Transform>(), x, z);
        return querySurface(self->getComponent<TerrainComponent>(), local.x, local.y).inside;
    }

    RayReturn terrainLuaRaycast(Terrain* self, const Vector3& origin, const Vector3& direction, float maxDistance) {
        RayReturn out = Ray::NO_HIT;
        const float length = direction.length();
        if (length <= 0.0f || !(maxDistance > 0.0f)) return out;
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        const Transform& transform = self->getComponent<Transform>();
        Vector3 point;
        if (!raycastTerrain(terrain, transform, Ray(origin, direction * (maxDistance / length)), true, point)) return out;
        out.hit = true;
        out.point = point;
        out.distance = (point - origin).length();
        const Vector2 local = terrainLuaLocal(transform, point.x, point.z);
        out.normal = terrainLuaWorldNormal(transform, querySurface(terrain, local.x, local.y).normal);
        out.body = self->getEntity();
        out.colliderIndex = 0;
        return out;
    }

    terrain::MapFormat terrainLuaHeightFormat(Terrain* self) {
        TerrainComponentStore store(self->getScene(), self->getEntity(), self->getComponent<TerrainComponent>());
        return store.format(MapId{MapKind::Height, 0});
    }

    // Pushes the reason and returns false when the terrain may not be written (R75, R77)
    bool terrainLuaWritable(lua_State* L, Terrain* self, const char* function) {
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        const std::string name = self->getScene()->getEntityName(self->getEntity());
        if (!terrain.allowRuntimeEdits) {
            const std::string message = std::string("Terrain:") + function + ": \"" + name +
                                        "\" does not allow runtime edits; turn on Allow Runtime Edits (allowRuntimeEdits) on its Terrain component in the editor.";
            lua_pushstring(L, message.c_str());
            return false;
        }
        const std::string refusal = terrainRuntimeEditRefusal(terrain);
        if (!refusal.empty()) {
            const std::string message = std::string("Terrain:") + function + ": \"" + name + "\" cannot be edited at runtime on this device: " + refusal + ".";
            lua_pushstring(L, message.c_str());
            return false;
        }
        if (!detachTerrainMapsForRuntime(terrain, self->getEntity())) {
            const std::string message = std::string("Terrain:") + function + ": the maps of \"" + name + "\" could not be copied for runtime edits (out of memory).";
            lua_pushstring(L, message.c_str());
            return false;
        }
        return true;
    }

    // Arguments are checked before any C++ object exists (luaL_check* raises)
    TexelRect terrainLuaRectArgs(lua_State* L, int first) {
        return TexelRect{static_cast<int>(luaL_checkinteger(L, first)), static_cast<int>(luaL_checkinteger(L, first + 1)),
                         static_cast<int>(luaL_checkinteger(L, first + 2)), static_cast<int>(luaL_checkinteger(L, first + 3))};
    }

    bool terrainLuaRect(lua_State* L, const terrain::MapFormat& format, const TexelRect& rect, const char* function) {
        if (!format.exists) {
            lua_pushfstring(L, "Terrain:%s: the terrain has no height map loaded", function);
            return false;
        }
        if (rect.w <= 0 || rect.h <= 0 || rect.x < 0 || rect.y < 0 || rect.x + rect.w > format.bounds.w || rect.y + rect.h > format.bounds.h) {
            lua_pushfstring(L, "Terrain:%s: the rectangle (%d, %d, %d, %d) is outside the height map, which is %d x %d texels", function,
                            rect.x, rect.y, rect.w, rect.h, format.bounds.w, format.bounds.h);
            return false;
        }
        return true;
    }

    // terrain:getHeights(x, y, w, h): w * h terrain-local heights, row by row
    bool terrainLuaGetHeights(lua_State* L) {
        Terrain* self = terrainLuaSelf(L);
        const TexelRect rect = terrainLuaRectArgs(L, 2);
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        TerrainComponentStore store(self->getScene(), self->getEntity(), terrain);
        const MapId height{MapKind::Height, 0};
        if (!terrainLuaRect(L, store.format(height), rect, "getHeights")) return false;
        std::vector<float> values(static_cast<size_t>(rect.area()));
        store.read(height, rect, values);
        lua_createtable(L, static_cast<int>(values.size()), 0);
        for (size_t i = 0; i < values.size(); i++) {
            lua_pushnumber(L, static_cast<lua_Number>(values[i] * terrain.maxHeight));
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return true;
    }

    // terrain:setHeights(x, y, w, h, values): absolute terrain-local heights; the result table
    bool terrainLuaSetHeights(lua_State* L) {
        Terrain* self = terrainLuaSelf(L);
        const TexelRect rect = terrainLuaRectArgs(L, 2);
        luaL_checktype(L, 6, LUA_TTABLE);
        if (!terrainLuaWritable(L, self, "setHeights")) return false;
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        TerrainComponentStore store(self->getScene(), self->getEntity(), terrain);
        if (!terrainLuaRect(L, store.format(MapId{MapKind::Height, 0}), rect, "setHeights")) return false;
        const size_t count = static_cast<size_t>(rect.area());
        if (lua_rawlen(L, 6) != count) {
            lua_pushfstring(L, "Terrain:setHeights: values has %d numbers, the rectangle needs %d", static_cast<int>(lua_rawlen(L, 6)), static_cast<int>(count));
            return false;
        }
        std::vector<float> values(count);
        for (size_t i = 0; i < count; i++) {
            lua_rawgeti(L, 6, static_cast<int>(i + 1));
            const bool number = lua_type(L, -1) == LUA_TNUMBER;
            values[i] = number ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            if (!number || !std::isfinite(values[i])) {
                lua_pushfstring(L, "Terrain:setHeights: values[%d] is not a finite number", static_cast<int>(i + 1));
                return false;
            }
        }
        const terrain::TerrainOpResult result = terrain::setHeights(store, rect, values);
        if (!result.ok) {
            lua_pushstring(L, ("Terrain:setHeights: " + result.error).c_str());
            return false;
        }
        const terrain::OpSpace local = terrain::OpSpace::of(Matrix4(), Vector3(0.0f), Vector3(1.0f), false, false);
        LuaJson::push(L, terrain::opResultJson(result, local, {}, 1));
        return true;
    }

    // terrain:applyOps(ops): edit_terrain's op tables in world space; the result table
    bool terrainLuaApplyOps(lua_State* L) {
        Terrain* self = terrainLuaSelf(L);
        luaL_checktype(L, 2, LUA_TTABLE);
        nlohmann::json list;
        std::string error;
        if (!LuaJson::toJson(L, 2, list, LuaJson::EncodeOptions(), error)) {
            lua_pushstring(L, ("Terrain:applyOps: " + error).c_str());
            return false;
        }
        if (!terrainLuaWritable(L, self, "applyOps")) return false;
        TerrainComponent& terrain = self->getComponent<TerrainComponent>();
        const Transform& transform = self->getComponent<Transform>();
        const terrain::OpSpace space = terrain::OpSpace::of(transform.modelMatrix, transform.worldPosition, transform.worldScale,
                                                            transform.worldRotation != Quaternion::IDENTITY, true);
        terrain::OpJsonHooks hooks;
        hooks.fieldsDoc = "docs/scripting/terrain.md";
        hooks.firstIndex = 1;
        const std::string authoring = "is for the editor only: edit_terrain or the Terrain window.";
        hooks.refused = {{terrain::OpType::PlaceObjects, authoring}, {terrain::OpType::EraseObjects, authoring},
                         {terrain::OpType::RemapLayers, authoring}, {terrain::OpType::ImportHeightmap, authoring}};
        terrain::OpJsonParser parser(space, hooks);
        std::vector<terrain::Op> ops;
        if (!parser.parseList(list, ops)) {
            lua_pushstring(L, ("Terrain:applyOps: " + parser.error).c_str());
            return false;
        }
        TerrainComponentStore store(self->getScene(), self->getEntity(), terrain);
        const terrain::TerrainOpResult result = terrain::applyOps(store, ops);
        if (!result.ok) {
            lua_pushstring(L, ("Terrain:applyOps: " + result.error).c_str());
            return false;
        }
        LuaJson::push(L, terrain::opResultJson(result, space, ops, 1));
        return true;
    }

}

void LuaBinding::registerTerrainClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .beginClass<Terrain>("Terrain")
        .addFunction("getHeightAt", &terrainLuaHeightAt)
        .addFunction("getNormalAt", &terrainLuaNormalAt)
        .addFunction("isHoleAt", &terrainLuaIsHoleAt)
        .addFunction("contains", &terrainLuaContains)
        .addFunction("raycast", &terrainLuaRaycast)
        .addProperty("heightMapWidth", +[](const Terrain* self) { return terrainLuaHeightFormat(const_cast<Terrain*>(self)).bounds.w; })
        .addProperty("heightMapHeight", +[](const Terrain* self) { return terrainLuaHeightFormat(const_cast<Terrain*>(self)).bounds.h; })
        .addProperty("allowRuntimeEdits",
            +[](const Terrain* self) { return self->getComponent<TerrainComponent>().allowRuntimeEdits; },
            +[](Terrain* self, bool allow) { self->getComponent<TerrainComponent>().allowRuntimeEdits = allow; })
        .addFunction("setRuntimeUploadBudget", +[](Terrain* self, lua_Integer bytes) {
            TerrainComponent& terrain = self->getComponent<TerrainComponent>();
            if (!terrain.editState) terrain.editState = std::make_shared<TerrainEditState>();
            terrain.editState->uploadBudget = bytes > 0 ? static_cast<size_t>(bytes) : 0;
        })
        .addFunction("getHeights", +[](lua_State* L) -> int { return terrainLuaGetHeights(L) ? 1 : lua_error(L); })
        .addFunction("setHeights", +[](lua_State* L) -> int { return terrainLuaSetHeights(L) ? 1 : lua_error(L); })
        .addFunction("applyOps", +[](lua_State* L) -> int { return terrainLuaApplyOps(L) ? 1 : lua_error(L); })
        .endClass();
}
