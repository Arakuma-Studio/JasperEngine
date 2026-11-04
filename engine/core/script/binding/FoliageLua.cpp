// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Foliage in Lua (T5 F14, R30-R33, R20; docs/scripting/foliage.md): Terrain Settings as properties,
// runtime removal and its overlay, queries and hit events. A reopened Terrain, so the GameObject
// bindings stay as they are. All indices are 0-based.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "math/Ray.h"
#include "object/Terrain.h"
#include "picking/MeshRaycast.h"
#include "subsystem/FoliageSystem.h"

#include <string>

using namespace jasperengine;

namespace {

    TerrainComponent& terrainOf(const Terrain* self) {
        return self->getScene()->getComponent<TerrainComponent>(self->getEntity());
    }

    FoliageSystem* foliageOf(const Terrain* self) {
        return self->getScene()->getSystem<FoliageSystem>().get();
    }

    luabridge::LuaRef poseTable(lua_State* L, const FoliageInstanceInfo& info) {
        luabridge::LuaRef table = luabridge::newTable(L);
        table["id"] = static_cast<lua_Integer>(info.id);
        table["kind"] = info.tree ? "trees" : "details";
        if (info.tree) table["prototype"] = info.prototype;
        else table["layer"] = info.layer;
        table["position"] = info.position;
        table["rotation"] = info.rotation;
        table["scale"] = info.scale;
        return table;
    }

    // Each terrain's Lua handlers die with its scene: its FoliageSystem holds them
    void setTreeHitHandler(Terrain* self, const luabridge::LuaRef& function, lua_State* L) {
        FoliageSystem* foliage = foliageOf(self);
        const Entity entity = self->getEntity();
        if (function.isNil() || !function.isFunction()) {
            foliage->setTreeHitHandler(entity, nullptr);
            return;
        }
        luabridge::LuaRef kept = function;
        foliage->setTreeHitHandler(entity, [kept, L](const TreeHitEvent& hit) {
            luabridge::LuaRef table = luabridge::newTable(L);
            table["id"] = static_cast<lua_Integer>(hit.id);
            table["prototype"] = hit.prototype;
            table["point"] = hit.point;
            table["normal"] = hit.normal;
            table["body"] = static_cast<lua_Integer>(hit.body);
            table["speed"] = hit.speed;
            try {
                kept(table);
            } catch (const luabridge::LuaException& error) {
                Log::error("onTreeHit: %s", error.what());
            }
        });
    }

    void setTreeRemovedHandler(Terrain* self, const luabridge::LuaRef& function, lua_State* L) {
        FoliageSystem* foliage = foliageOf(self);
        const Entity entity = self->getEntity();
        if (function.isNil() || !function.isFunction()) {
            foliage->setTreeRemovedHandler(entity, nullptr);
            return;
        }
        luabridge::LuaRef kept = function;
        foliage->setTreeRemovedHandler(entity, [kept, L](terrain::TreeId id, const FoliageInstanceInfo& pose) {
            try {
                kept(static_cast<lua_Integer>(id), poseTable(L, pose));
            } catch (const luabridge::LuaException& error) {
                Log::error("onTreeRemoved: %s", error.what());
            }
        });
    }

}

void LuaBinding::registerFoliageClasses(lua_State* L) {
    luabridge::getGlobalNamespace(L)
        .beginClass<Terrain>("Terrain")
        .addProperty("detailDistance", +[](const Terrain* self) { return terrainOf(self).detailDistance; }, +[](Terrain* self, float value) { terrainOf(self).detailDistance = value; terrainOf(self).needUpdateFoliage = true; })
        .addProperty("detailDensity", +[](const Terrain* self) { return terrainOf(self).detailDensity; }, +[](Terrain* self, float value) { terrainOf(self).detailDensity = value; })
        .addProperty("treeDistance", +[](const Terrain* self) { return terrainOf(self).treeDistance; }, +[](Terrain* self, float value) { terrainOf(self).treeDistance = value; })
        .addProperty("billboardStart", +[](const Terrain* self) { return terrainOf(self).billboardStart; }, +[](Terrain* self, float value) { terrainOf(self).billboardStart = value; })
        .addProperty("fadeLength", +[](const Terrain* self) { return terrainOf(self).fadeLength; }, +[](Terrain* self, float value) { terrainOf(self).fadeLength = value; })
        .addProperty("maxMeshTrees", +[](const Terrain* self) { return terrainOf(self).maxMeshTrees; }, +[](Terrain* self, int value) { terrainOf(self).maxMeshTrees = value; })
        .addProperty("windDirection", +[](const Terrain* self) { return terrainOf(self).windDirection; }, +[](Terrain* self, Vector3 value) { terrainOf(self).windDirection = value; })
        .addProperty("windSpeed", +[](const Terrain* self) { return terrainOf(self).windSpeed; }, +[](Terrain* self, float value) { terrainOf(self).windSpeed = value; })
        .addProperty("windSize", +[](const Terrain* self) { return terrainOf(self).windSize; }, +[](Terrain* self, float value) { terrainOf(self).windSize = value; })
        .addProperty("windBending", +[](const Terrain* self) { return terrainOf(self).windBending; }, +[](Terrain* self, float value) { terrainOf(self).windBending = value; })
        .addFunction("setDetailDensity", +[](Terrain* self, int layer, float value) {
            TerrainComponent& terrain = terrainOf(self);
            if (layer >= 0 && layer < static_cast<int>(terrain.foliageLayers.size())) terrain.foliageLayers[layer].densityScale = value;
        })
        .addFunction("getFoliageInstancesInRadius", +[](Terrain* self, Vector3 position, float radius, lua_State* L) -> luabridge::LuaRef {
            int kind = 0;
            size_t maxResults = 256;
            // the optional fourth argument
            if (lua_istable(L, 4)) {
                luabridge::LuaRef options = luabridge::LuaRef::fromStack(L, 4);
                const luabridge::LuaRef kindOption = options["kind"];
                if (kindOption.isString()) {
                    const std::string text = kindOption.unsafe_cast<std::string>();
                    kind = text == "details" ? 1 : text == "all" ? 2 : 0;
                }
                const luabridge::LuaRef maxOption = options["max"];
                if (maxOption.isNumber()) maxResults = static_cast<size_t>(maxOption.unsafe_cast<int>());
            }
            luabridge::LuaRef results = luabridge::newTable(L);
            const std::vector<FoliageInstanceInfo> found = foliageOf(self)->getInstancesInRadius(self->getEntity(), position, radius, kind, maxResults);
            for (size_t i = 0; i < found.size(); i++) results[static_cast<int>(i) + 1] = poseTable(L, found[i]);
            return results;
        })
        .addFunction("removeTreeInstance", +[](Terrain* self, lua_Integer id, lua_State* L) -> luabridge::LuaRef {
            bool updateNavigation = false;
            if (lua_istable(L, 3)) {
                luabridge::LuaRef options = luabridge::LuaRef::fromStack(L, 3);
                const luabridge::LuaRef option = options["updateNavigation"];
                if (option.isBool()) updateNavigation = option.unsafe_cast<bool>();
            }
            const std::optional<FoliageInstanceInfo> pose = foliageOf(self)->removeTreeInstance(self->getEntity(), static_cast<terrain::TreeId>(id), updateNavigation);
            if (!pose) return luabridge::LuaRef(L);
            return poseTable(L, *pose);
        })
        .addFunction("restoreTreeInstance", +[](Terrain* self, lua_Integer id) {
            return foliageOf(self)->restoreTreeInstance(self->getEntity(), static_cast<terrain::TreeId>(id));
        })
        .addFunction("addTreeInstance", +[](Terrain* self, const luabridge::LuaRef& spec) -> lua_Integer {
            if (!spec.isTable()) return 0;
            const luabridge::LuaRef prototype = spec["prototype"];
            const luabridge::LuaRef position = spec["position"];
            const luabridge::LuaRef yaw = spec["yaw"];
            const luabridge::LuaRef scale = spec["scale"];
            if (!prototype.isNumber() || !position.isUserdata()) return 0;
            return static_cast<lua_Integer>(foliageOf(self)->addTreeInstance(self->getEntity(), prototype.unsafe_cast<int>(), position.unsafe_cast<Vector3>(),
                yaw.isNumber() ? yaw.unsafe_cast<float>() : 0.0f, scale.isNumber() ? scale.unsafe_cast<float>() : 1.0f));
        })
        .addFunction("clearDetailsInRadius", +[](Terrain* self, Vector3 position, float radius, lua_State* L) {
            const int layer = lua_isnumber(L, 4) ? static_cast<int>(lua_tointeger(L, 4)) : -1;
            foliageOf(self)->clearDetailsInRadius(self->getEntity(), position, radius, layer);
        })
        .addFunction("restoreDetails", +[](Terrain* self) { foliageOf(self)->restoreDetails(self->getEntity()); })
        .addFunction("getRuntimeFoliageState", +[](const Terrain* self) { return foliageOf(self)->getRuntimeState(self->getEntity()); })
        .addFunction("setRuntimeFoliageState", +[](Terrain* self, const std::string& state) { return foliageOf(self)->setRuntimeState(self->getEntity(), state); })
        .addProperty("onTreeHit", +[](const Terrain*, lua_State* L) { return luabridge::LuaRef(L); }, +[](Terrain* self, const luabridge::LuaRef& function, lua_State* L) { setTreeHitHandler(self, function, L); })
        .addProperty("onTreeRemoved", +[](const Terrain*, lua_State* L) { return luabridge::LuaRef(L); }, +[](Terrain* self, const luabridge::LuaRef& function, lua_State* L) { setTreeRemovedHandler(self, function, L); })
        // a MeshRaycast hit, a Physics ray hit or a contact's body and collider: its tree, or 0
        .addFunction("getTreeFromHit", +[](Terrain* self, const luabridge::LuaRef& hit) -> lua_Integer {
            FoliageSystem* foliage = foliageOf(self);
            if (hit.isInstance<MeshRaycastHit>()) {
                return static_cast<lua_Integer>(hit.unsafe_cast<MeshRaycastHit>().foliageId);
            }
            if (hit.isInstance<RayReturn>()) {
                const RayReturn ray = hit.unsafe_cast<RayReturn>();
                Entity terrainEntity = NULL_ENTITY;
                terrain::TreeId id = 0;
                if (foliage->getTreeFromBody(ray.body, ray.colliderIndex, terrainEntity, id) && terrainEntity == self->getEntity()) return static_cast<lua_Integer>(id);
            }
            return 0;
        })
        .endClass();
}
