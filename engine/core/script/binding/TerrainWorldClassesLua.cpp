// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R47: Lua's TerrainWorld (docs/scripting/terrain-worlds.md), GameObject:getTerrainWorld and
// TerrainWorldSurface. Positions are world X and Z; a bad argument raises an error naming it.
//
// A focus a script adds belongs to the world's runtime, not to the script or to the Lua value:
// it stays until removeFocus, or until the runtime goes (the World GameObject destroyed, its
// Terrain World removed or disabled, the scene stopped or unloaded). A Lua value is collected
// at no set time and getTerrainWorld makes a new one each call, so tying a focus to it would
// drop focuses at random; a script whose GameObject outlives its need removes its focus itself.
//
// Lua is compiled as C: luaL_error and lua_yield longjmp over C++ frames. Checks build their
// message in a helper that returns, and the binding raises only once no C++ object is alive.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaFunction.h"

#include "Scene.h"
#include "component/TerrainWorldComponent.h"
#include "object/GameObject.h"
#include "object/TerrainWorld.h"
#include "script/ScriptScheduler.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>

// the Terrain World module (S34 R1): the Lua API description reads this condition
#if JASPERENGINE_TERRAIN_WORLD

using namespace jasperengine;

namespace {

    constexpr int WORLD_LUA_RAISE = -1;

    // the message the next raise carries: static, so nothing is left to destroy when it longjmps
    std::string& worldLuaMessage() {
        static std::string message;
        return message;
    }

    int worldLuaRaise(lua_State* L) {
        return luaL_error(L, "%s", worldLuaMessage().c_str());
    }

    bool worldLuaFail(const char* function, const std::string& why) {
        worldLuaMessage() = std::string("TerrainWorld:") + function + ": " + why;
        return false;
    }

    // The GameObject is a Terrain World (an answer about its tiles needs one)
    bool worldLuaValid(const TerrainWorld* self, const char* function) {
        if (self && self->isValid()) return true;
        return worldLuaFail(function, "the GameObject has no Terrain World component (go:getTerrainWorld() is nil for it)");
    }

    // The world's runtime: focuses and waits need its streaming
    bool worldLuaRuntime(const TerrainWorld* self, const char* function, TerrainWorldRuntime*& runtime) {
        runtime = nullptr;
        if (!worldLuaValid(self, function)) return false;
        std::string error;
        runtime = self->getRuntime(&error);
        if (runtime) return true;
        return worldLuaFail(function, "the Terrain World does not run: " + error);
    }

    // double: LuaBridge refuses an infinity as a float before the check could name it
    bool worldLuaFinite(const char* function, double x, double z) {
        if (std::isfinite(x) && std::isfinite(z)) return true;
        return worldLuaFail(function, "x and z must be finite numbers");
    }

    bool worldLuaLevel(const char* function, const char* level, TerrainWorldReadyLevel& out) {
        if (level && std::strcmp(level, "render") == 0) {
            out = TerrainWorldReadyLevel::Render;
            return true;
        }
        if (level && std::strcmp(level, "collision") == 0) {
            out = TerrainWorldReadyLevel::Collision;
            return true;
        }
        return worldLuaFail(function, std::string("level must be \"render\" or \"collision\", got ") +
                                      (level ? "\"" + std::string(level) + "\"" : std::string("nil")));
    }

    bool worldLuaArea(const char* function, double x, double z, double radius, const char* level, TerrainWorldReadyLevel& out) {
        if (!worldLuaFinite(function, x, z)) return false;
        if (!std::isfinite(radius) || radius < 0.0) return worldLuaFail(function, "radius must be a finite number >= 0");
        return worldLuaLevel(function, level, out);
    }

    // R18 walks the tiles of the circle's square: past the world's whole span (512 tiles a side)
    // an area would walk millions of them, and there is nothing there
    bool worldLuaAreaInWorld(const char* function, const TerrainWorld& self, const TerrainWorldRuntime& runtime, float x, float z, float radius) {
        const float span = static_cast<float>(terraincore::WORLD_TILES_PER_SIDE) * runtime.lattice().tileSize();
        const Vector3 origin = self.getOrigin();
        if (std::fabs(x - origin.x) <= span && std::fabs(z - origin.z) <= span && radius <= span) return true;
        return worldLuaFail(function, "the area must lie within " + std::to_string(static_cast<long long>(span)) +
                                      " m of the World and its radius be at most that (the world's whole span)");
    }

    bool worldLuaVector(const char* function, const char* name, const Vector3& value) {
        if (std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)) return true;
        return worldLuaFail(function, std::string(name) + " must be finite");
    }

    // A world that does not run has no area ready; that is no error, as isReady is not
    bool worldLuaIsAreaReady(const TerrainWorld* self, double x, double z, double radius, const char* level, bool& ready) {
        ready = false;
        TerrainWorldReadyLevel parsed = TerrainWorldReadyLevel::Render;
        if (!worldLuaValid(self, "isAreaReady") || !worldLuaArea("isAreaReady", x, z, radius, level, parsed)) return false;
        TerrainWorldRuntime* runtime = self->getRuntime();
        if (!runtime) return true;
        if (!worldLuaAreaInWorld("isAreaReady", *self, *runtime, static_cast<float>(x), static_cast<float>(z), static_cast<float>(radius))) return false;
        ready = self->isAreaReady(static_cast<float>(x), static_cast<float>(z), static_cast<float>(radius), parsed);
        return true;
    }

    // world:addFocus(position [, radiusScale [, velocity]]): its id
    bool worldLuaAddFocus(TerrainWorld* self, const Vector3& position, float radiusScale, const Vector3& velocity, int& id) {
        TerrainWorldRuntime* runtime = nullptr;
        if (!worldLuaRuntime(self, "addFocus", runtime)) return false;
        if (!worldLuaVector("addFocus", "position", position) || !worldLuaVector("addFocus", "velocity", velocity)) return false;
        if (!std::isfinite(radiusScale) || radiusScale <= 0.0f) return worldLuaFail("addFocus", "radiusScale must be a finite number > 0");
        id = self->addFocus(position, radiusScale, velocity);
        if (id != 0) return true;
        return worldLuaFail("addFocus", "a world has at most " + std::to_string(terraincore::WORLD_MAX_FOCUSES) +
                                        " focuses, the camera's and the component's included; remove one with removeFocus");
    }

    bool worldLuaSetFocus(TerrainWorld* self, int id, const Vector3& position, const Vector3& velocity) {
        TerrainWorldRuntime* runtime = nullptr;
        if (!worldLuaRuntime(self, "setFocus", runtime)) return false;
        if (!worldLuaVector("setFocus", "position", position) || !worldLuaVector("setFocus", "velocity", velocity)) return false;
        if (self->setFocus(id, position, velocity)) return true;
        return worldLuaFail("setFocus", "no focus " + std::to_string(id) + " (addFocus gives the id; it was removed, or the world restarted)");
    }

    bool worldLuaSetting(const TerrainWorld* self, const char* name, double value) {
        if (!worldLuaValid(self, name)) return false;
        if (std::isfinite(value) && value >= 0.0) return true;
        return worldLuaFail(name, "must be a finite number >= 0 (0: the platform's default)");
    }

    // getStats: R45's numbers, with the reason when the world does not run
    luabridge::LuaRef worldLuaStats(lua_State* L, const TerrainWorld& self) {
        luabridge::LuaRef table = luabridge::newTable(L);
        std::string error;
        TerrainWorldRuntime* runtime = self.getRuntime(&error);
        if (!runtime) {
            table["ready"] = false;
            table["running"] = false;
            table["error"] = error;
            return table;
        }
        const TerrainWorldStats stats = runtime->stats();
        table["running"] = true;
        table["ready"] = stats.ready;
        table["frozen"] = stats.frozen;
        table["tiles"] = stats.tiles;
        luabridge::LuaRef states = luabridge::newTable(L);
        for (int i = 0; i < 8; i++) states[terraincore::tileStateName(static_cast<terraincore::TileState>(i))] = stats.states[i];
        table["tileStates"] = states;
        table["focuses"] = runtime->focusCount();
        table["gpuBytes"] = static_cast<lua_Integer>(stats.gpuBytes);
        table["cpuBytes"] = static_cast<lua_Integer>(stats.cpuBytes);
        table["gpuCap"] = static_cast<lua_Integer>(stats.gpuCap);
        table["cpuCap"] = static_cast<lua_Integer>(stats.cpuCap);
        table["poolSize"] = stats.poolSize;
        table["queue"] = stats.queue;
        table["inFlight"] = stats.inFlight;
        table["uploadBytesLastFrame"] = static_cast<lua_Integer>(stats.uploadBytesLastFrame);
        table["uploadBytesTotal"] = static_cast<lua_Integer>(stats.uploadBytesTotal);
        table["uploadRetries"] = stats.uploadRetries;
        table["mainThreadMs"] = stats.mainThreadMs;
        table["worstMainThreadMs"] = stats.worstMainThreadMs;
        table["nodesVisited"] = stats.nodesVisited;
        table["nodesSelected"] = stats.nodesSelected;
        table["collisionFine"] = stats.collisionFine;
        table["collisionCoarse"] = stats.collisionCoarse;
        table["frozenBodies"] = stats.frozenBodies;
        table["seamErrors"] = stats.seamErrors;
        table["poolExhausted"] = stats.poolExhausted;
        table["latePages"] = stats.latePages;
        luabridge::LuaRef errors = luabridge::newTable(L);
        for (size_t i = 0; i < stats.errors.size(); i++) errors[static_cast<int>(i) + 1] = stats.errors[i];
        table["errors"] = errors;
        return table;
    }

    // ---------------------------------------------------------------- onAreaReady, waitForArea

    // x, z, radius, level at 2..5 of a raw function: false with the message, else the parsed area
    bool worldLuaAreaArgs(lua_State* L, const char* function, float& x, float& z, float& radius, TerrainWorldReadyLevel& level) {
        for (int i = 2; i <= 4; i++) {
            if (lua_type(L, i) != LUA_TNUMBER) return worldLuaFail(function, "x, z and radius must be numbers");
        }
        x = static_cast<float>(lua_tonumber(L, 2));
        z = static_cast<float>(lua_tonumber(L, 3));
        radius = static_cast<float>(lua_tonumber(L, 4));
        return worldLuaArea(function, x, z, radius, lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : nullptr, level);
    }

    TerrainWorld* worldLuaSelfAt(lua_State* L) {
        auto self = luabridge::get<TerrainWorld*>(L, 1);
        return self ? *self : nullptr;
    }

    // world:onAreaReady(x, z, radius, level, callback)
    bool worldLuaOnAreaReady(lua_State* L) {
        TerrainWorld* self = worldLuaSelfAt(L);
        if (!self) return worldLuaFail("onAreaReady", "call it with ':' on a TerrainWorld");
        float x = 0.0f, z = 0.0f, radius = 0.0f;
        TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
        if (!worldLuaAreaArgs(L, "onAreaReady", x, z, radius, level)) return false;
        if (lua_type(L, 6) != LUA_TFUNCTION) return worldLuaFail("onAreaReady", "callback must be a function");
        TerrainWorldRuntime* runtime = nullptr;
        if (!worldLuaRuntime(self, "onAreaReady", runtime)) return false;
        if (!worldLuaAreaInWorld("onAreaReady", *self, *runtime, x, z, radius)) return false;
        lua_pushvalue(L, 6);
        LuaFunction<void> callback(L);
        self->onAreaReady(x, z, radius, level, [callback]() mutable { callback(); });
        return true;
    }

    // waitForArea's pending area: a focus (R18) until this closes, which it does as waitForArea
    // returns or raises, or when its coroutine is stopped (a to-be-closed slot). The scene may be
    // gone by then (Play stopped before the coroutines were cleared): its lifetime says so.
    struct WorldLuaAreaWait {
        std::weak_ptr<char> sceneLifetime;
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        TerrainWorldRuntime* runtime = nullptr;
        int wait = 0;
        float x = 0.0f, z = 0.0f, radius = 0.0f;
        TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;

        // the runtime the wait was made on, while it still runs (a restart makes another, whose
        // wait ids start again)
        TerrainWorldRuntime* live() const {
            if (!scene || sceneLifetime.expired()) return nullptr;
            std::shared_ptr<TerrainWorldSystem> system = scene->findSystem<TerrainWorldSystem>();
            TerrainWorldRuntime* now = system ? system->getRuntime(entity) : nullptr;
            return now == runtime ? now : nullptr;
        }

        void drop() {
            if (wait == 0) return;
            if (TerrainWorldRuntime* now = live()) now->dropAreaWait(wait);
            wait = 0;
        }
    };

    const char* const WORLD_LUA_AREA_WAIT = "jasperengine.TerrainWorldAreaWait";

    int worldLuaAreaWaitClose(lua_State* L) {
        if (auto* wait = static_cast<WorldLuaAreaWait*>(lua_touserdata(L, 1))) wait->drop();
        return 0;
    }

    int worldLuaAreaWaitGc(lua_State* L) {
        if (auto* wait = static_cast<WorldLuaAreaWait*>(lua_touserdata(L, 1))) {
            wait->drop();
            wait->~WorldLuaAreaWait();
        }
        return 0;
    }

    // Coroutine.waitUntil's predicate: the area is ready, or never will be (the world went)
    int worldLuaAreaWaitReady(lua_State* L) {
        const auto* wait = static_cast<const WorldLuaAreaWait*>(lua_touserdata(L, lua_upvalueindex(1)));
        TerrainWorldRuntime* runtime = wait ? wait->live() : nullptr;
        lua_pushboolean(L, !runtime || runtime->isAreaReady(wait->x, wait->z, wait->radius, wait->level));
        return 1;
    }

    // The checks and the area's wait: WORLD_LUA_RAISE, 1 with true pushed when the area is
    // ready now, or 0 with the wait's userdata on top (index 7)
    int worldLuaWaitForAreaBegin(lua_State* L) {
        TerrainWorld* self = worldLuaSelfAt(L);
        if (!self) {
            worldLuaFail("waitForArea", "call it with ':' on a TerrainWorld");
            return WORLD_LUA_RAISE;
        }
        float x = 0.0f, z = 0.0f, radius = 0.0f;
        TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
        if (!worldLuaAreaArgs(L, "waitForArea", x, z, radius, level)) return WORLD_LUA_RAISE;
        if (!lua_isnoneornil(L, 6) && (lua_type(L, 6) != LUA_TNUMBER || !(lua_tonumber(L, 6) >= 0.0))) {
            worldLuaFail("waitForArea", "timeoutSeconds must be a number of seconds >= 0, or nil to wait for as long as it takes");
            return WORLD_LUA_RAISE;
        }
        if (!lua_isyieldable(L)) {
            worldLuaFail("waitForArea", "not inside an engine coroutine (start one with Coroutine.start or self:startCoroutine); "
                                        "outside one, use onAreaReady or isAreaReady");
            return WORLD_LUA_RAISE;
        }
        TerrainWorldRuntime* runtime = nullptr;
        if (!worldLuaRuntime(self, "waitForArea", runtime)) return WORLD_LUA_RAISE;
        if (!worldLuaAreaInWorld("waitForArea", *self, *runtime, x, z, radius)) return WORLD_LUA_RAISE;
        const Vector3 origin = self->getOrigin();
        const float localX = x - origin.x, localZ = z - origin.z;
        if (runtime->isAreaReady(localX, localZ, radius, level)) {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_settop(L, 6);
        // the metatable first: a userdata without its __gc would leak the weak pointer
        auto* wait = new (lua_newuserdatauv(L, sizeof(WorldLuaAreaWait), 0)) WorldLuaAreaWait();
        if (luaL_newmetatable(L, WORLD_LUA_AREA_WAIT)) {
            lua_pushcfunction(L, &worldLuaAreaWaitClose);
            lua_setfield(L, -2, "__close");
            lua_pushcfunction(L, &worldLuaAreaWaitGc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        wait->scene = self->getScene();
        wait->sceneLifetime = wait->scene->getLifetime();
        wait->entity = self->getEntity();
        wait->runtime = runtime;
        wait->x = localX;
        wait->z = localZ;
        wait->radius = radius;
        wait->level = level;
        wait->wait = runtime->waitForArea(localX, localZ, radius, level);
        return 0;
    }

    // After Coroutine.waitUntil: true when the area is ready, false at the timeout or when the
    // world went; returning closes the wait's slot
    int worldLuaWaitForAreaDone(lua_State* L, int status, lua_KContext ctx) {
        (void)status;
        const auto* wait = static_cast<const WorldLuaAreaWait*>(lua_touserdata(L, static_cast<int>(ctx)));
        TerrainWorldRuntime* runtime = wait ? wait->live() : nullptr;
        lua_pushboolean(L, runtime && runtime->isAreaReady(wait->x, wait->z, wait->radius, wait->level));
        return 1;
    }

    // world:waitForArea(x, z, radius, level [, timeoutSeconds]) in an engine coroutine: waits in
    // Coroutine.waitUntil, so the scheduler's rules (who resumes, pauses, stops) are its own
    int worldLuaWaitForArea(lua_State* L) {
        const int begun = worldLuaWaitForAreaBegin(L);
        if (begun == WORLD_LUA_RAISE) return worldLuaRaise(L);
        if (begun > 0) return begun;
        const int guard = lua_gettop(L);
        lua_toclose(L, guard);
        lua_pushcfunction(L, &ScriptSchedulerLua::waitUntil);
        lua_pushvalue(L, guard);
        lua_pushcclosure(L, &worldLuaAreaWaitReady, 1);
        lua_pushvalue(L, 6);
        lua_callk(L, 2, 1, static_cast<lua_KContext>(guard), &worldLuaWaitForAreaDone);
        return worldLuaWaitForAreaDone(L, LUA_OK, static_cast<lua_KContext>(guard));
    }

}

void LuaBinding::registerTerrainWorldClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginClass<TerrainWorldSurface>("TerrainWorldSurface")
        // world Y of the surface
        .addProperty("height", &TerrainWorldSurface::height)
        .addProperty("normal", &TerrainWorldSurface::normal)
        .addProperty("hole", &TerrainWorldSurface::hole)
        // a tile of the world covers the point
        .addProperty("present", &TerrainWorldSurface::present)
        // from the tile's own samples; false: from the overview, the tile not in memory
        .addProperty("exact", &TerrainWorldSurface::exact)
        // "absent", "overview" or "full"
        .addProperty("state", &TerrainWorldSurface::state)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<TerrainWorld, EntityHandle>("TerrainWorld")
        .addConstructor<void (Scene*, Entity)>()
        // a focus streams the tiles round it (radiusScale times the near band); its id. At most 8 a
        // world, the camera's and the component's included. It stays until removeFocus or the world stops.
        .addFunction("addFocus", +[](TerrainWorld* self, const Vector3& position, std::optional<float> radiusScale,
                                     std::optional<Vector3> velocity, lua_State* L) -> int {
            int id = 0;
            if (!worldLuaAddFocus(self, position, radiusScale.value_or(1.0f), velocity.value_or(Vector3(0.0f, 0.0f, 0.0f)), id))
                worldLuaRaise(L);
            return id;
        })
        // moves a focus addFocus made (the velocity predicts where it streams next)
        .addFunction("setFocus", +[](TerrainWorld* self, int id, const Vector3& position, std::optional<Vector3> velocity, lua_State* L) {
            if (!worldLuaSetFocus(self, id, position, velocity.value_or(Vector3(0.0f, 0.0f, 0.0f))))
                worldLuaRaise(L);
        })
        // false when no focus has that id
        .addFunction("removeFocus", +[](TerrainWorld* self, int id, lua_State* L) -> bool {
            if (!worldLuaValid(self, "removeFocus")) worldLuaRaise(L);
            return self->removeFocus(id);
        })
        // never waits for a tile in a game: off a tile in memory it answers from the overview (exact false)
        .addFunction("getSurfaceAt", +[](TerrainWorld* self, double x, double z, lua_State* L) -> TerrainWorldSurface {
            if (!worldLuaValid(self, "getSurfaceAt") || !worldLuaFinite("getSurfaceAt", x, z)) worldLuaRaise(L);
            return self->getSurfaceAt(static_cast<float>(x), static_cast<float>(z));
        })
        // the state of the tile under the point: "absent", "cold", "queued", "loading", "cpu_ready",
        // "uploading", "resident" or "error"
        .addFunction("getTileState", +[](TerrainWorld* self, double x, double z, lua_State* L) -> std::string {
            if (!worldLuaValid(self, "getTileState") || !worldLuaFinite("getTileState", x, z)) worldLuaRaise(L);
            return self->getTileState(static_cast<float>(x), static_cast<float>(z));
        })
        // level "render": every present tile touching the circle draws at full detail;
        // "collision": every one has its fine colliders
        .addFunction("isAreaReady", +[](TerrainWorld* self, double x, double z, double radius, const char* level, lua_State* L) -> bool {
            bool ready = false;
            if (!worldLuaIsAreaReady(self, x, z, radius, level, ready)) worldLuaRaise(L);
            return ready;
        })
        .addFunction("waitForArea", &worldLuaWaitForArea)
        .addFunction("onAreaReady", +[](lua_State* L) -> int { return worldLuaOnAreaReady(L) ? 0 : worldLuaRaise(L); })
        // a coarse collider under the point now, before teleporting there; false off the world's tiles
        .addFunction("ensureCollisionAt", +[](TerrainWorld* self, double x, double z, lua_State* L) -> bool {
            if (!worldLuaValid(self, "ensureCollisionAt") || !worldLuaFinite("ensureCollisionAt", x, z)) worldLuaRaise(L);
            return self->ensureCollisionAt(static_cast<float>(x), static_cast<float>(z));
        })
        // the overview is in, so every present tile draws
        .addFunction("isReady", +[](TerrainWorld* self, lua_State* L) -> bool {
            if (!worldLuaValid(self, "isReady")) worldLuaRaise(L);
            return self->isReady();
        })
        // the streaming's numbers (TerrainWorldStats); running false with error when the world does not run
        .addFunction("getStats", +[](TerrainWorld* self, lua_State* L) -> luabridge::LuaRef {
            if (!worldLuaValid(self, "getStats")) worldLuaRaise(L);
            return worldLuaStats(L, *self);
        })
        // resident pages; 0: the platform's default
        .addProperty("poolSize",
            +[](const TerrainWorld* self, lua_State* L) -> int {
                if (!worldLuaValid(self, "poolSize")) worldLuaRaise(L);
                return self->getPoolSize();
            },
            +[](TerrainWorld* self, int poolSize, lua_State* L) {
                if (!worldLuaSetting(self, "poolSize", static_cast<double>(poolSize))) worldLuaRaise(L);
                self->setPoolSize(poolSize);
            })
        // the near band's multiplier; 0: the platform's default
        .addProperty("lodDistance",
            +[](const TerrainWorld* self, lua_State* L) -> float {
                if (!worldLuaValid(self, "lodDistance")) worldLuaRaise(L);
                return self->getLodDistance();
            },
            +[](TerrainWorld* self, double lodDistance, lua_State* L) {
                if (!worldLuaSetting(self, "lodDistance", lodDistance)) worldLuaRaise(L);
                self->setLodDistance(static_cast<float>(lodDistance));
            })
        // MiB uploaded a frame; 0: the platform's default
        .addProperty("uploadBudget",
            +[](const TerrainWorld* self, lua_State* L) -> float {
                if (!worldLuaValid(self, "uploadBudget")) worldLuaRaise(L);
                return self->getUploadBudget();
            },
            +[](TerrainWorld* self, double uploadBudget, lua_State* L) {
                if (!worldLuaSetting(self, "uploadBudget", uploadBudget)) worldLuaRaise(L);
                self->setUploadBudget(static_cast<float>(uploadBudget));
            })
        // metres of fine collision round each focus; 0: the platform's default
        .addProperty("collisionRadius",
            +[](const TerrainWorld* self, lua_State* L) -> float {
                if (!worldLuaValid(self, "collisionRadius")) worldLuaRaise(L);
                return self->getCollisionRadius();
            },
            +[](TerrainWorld* self, double collisionRadius, lua_State* L) {
                if (!worldLuaSetting(self, "collisionRadius", collisionRadius)) worldLuaRaise(L);
                self->setCollisionRadius(static_cast<float>(collisionRadius));
            })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<GameObject>("GameObject")
        // the GameObject's Terrain World, nil when it has none
        .addFunction("getTerrainWorld", +[](GameObject* self) -> std::optional<TerrainWorld> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<TerrainWorldComponent>(self->getEntity())){
                return std::nullopt;
            }
            return TerrainWorld(scene, self->getEntity());
        })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_TERRAIN_WORLD
