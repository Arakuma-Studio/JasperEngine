// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Culling Groups and Simulation Culling in Lua (docs/specs/culling-group-and-simulation-
// distance.md R33, docs/scripting/culling-groups.md): CullingGroup(scene), BoundingSphere and
// SimulationCulling(scene, entity) with its scene-wide functions. Sphere indices are 1-based here;
// bands are 0-based (0 nearest, -1 unknown). onStateChanged gets five values (index, isVisible,
// wasVisible, band, previousBand), no table; queryIndices fills a table the script keeps.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames. Checks write their message into a
// static buffer and the binding raises only once no C++ object is alive.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "script/binding/SceneConstructors.h"
#include "LuaFunction.h"

#include "Scene.h"
#include "object/CullingGroup.h"
#include "object/EntityHandle.h"
#include "object/SimulationCulling.h"
#include "subsystem/CullingSystem.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    char cullingLuaMessage[512];

    bool cullingLuaFail(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(cullingLuaMessage, sizeof(cullingLuaMessage), format, args);
        va_end(args);
        return false;
    }

    int cullingLuaRaise(lua_State* L) {
        return luaL_error(L, "%s", cullingLuaMessage);
    }

    CullingGroup* cullingLuaSelf(lua_State* L, const char* function) {
        CullingGroup* group = nullptr;
        if (luabridge::isInstance<CullingGroup>(L, 1)) {
            auto result = luabridge::get<CullingGroup*>(L, 1);
            if (result) group = result.value();
        }
        if (!group) cullingLuaFail("CullingGroup:%s: call it with ':' on a CullingGroup", function);
        return group;
    }

    // a sphere index (1-based) of the group, as 0-based
    bool cullingLuaIndex(lua_State* L, int arg, const CullingGroup& group, const char* function, uint32_t& index) {
        if (!lua_isinteger(L, arg)) return cullingLuaFail("CullingGroup:%s: the sphere index must be an integer", function);
        const lua_Integer value = lua_tointeger(L, arg);
        const uint32_t count = group.getBoundingSphereCount();
        if (value < 1 || value > static_cast<lua_Integer>(count)) {
            return cullingLuaFail("CullingGroup:%s: sphere %lld is not in the group (1..%u; setBoundingSphereCount adds spheres)", function,
                                  static_cast<long long>(value), count);
        }
        index = static_cast<uint32_t>(value - 1);
        return true;
    }

    // a GameObject (any EntityHandle) or its numeric id
    bool cullingLuaEntity(lua_State* L, int arg, Entity& entity) {
        if (lua_isinteger(L, arg)) {
            entity = static_cast<Entity>(lua_tointeger(L, arg));
            return true;
        }
        if (luabridge::isInstance<EntityHandle>(L, arg)) {
            auto result = luabridge::get<EntityHandle*>(L, arg);
            if (result && result.value()) {
                entity = result.value()->getEntity();
                return true;
            }
        }
        return false;
    }

    bool cullingLuaVector(lua_State* L, int arg, Vector3& out) {
        if (!luabridge::isInstance<Vector3>(L, arg)) return false;
        out = luabridge::get<Vector3>(L, arg).value();
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    bool cullingLuaSphere(lua_State* L, int arg, BoundingSphere& out) {
        if (!luabridge::isInstance<BoundingSphere>(L, arg)) return false;
        out = luabridge::get<BoundingSphere>(L, arg).value();
        return true;
    }

    Scene* cullingLuaScene(lua_State* L, int arg) {
        if (!lua_isuserdata(L, arg)) return nullptr;
        auto result = luabridge::get<Scene*>(L, arg);
        return result ? result.value() : nullptr;
    }

    // ---- CullingGroup

    // group:setSphereTarget(index, gameObject | entity, radius [, offset])
    int luaSetSphereTarget(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "setSphereTarget");
            uint32_t index = 0;
            if (!group || !cullingLuaIndex(L, 2, *group, "setSphereTarget", index)) return false;
            Entity entity = NULL_ENTITY;
            if (!cullingLuaEntity(L, 3, entity) || !group->getScene() || !group->getScene()->isEntityCreated(entity)) {
                return cullingLuaFail("CullingGroup:setSphereTarget: argument 2 must be a GameObject (or its entity id) of the group's scene");
            }
            const double radius = luaL_optnumber(L, 4, 0.0);
            if (!std::isfinite(radius) || radius < 0.0) return cullingLuaFail("CullingGroup:setSphereTarget: radius must be a finite number >= 0");
            Vector3 offset(0, 0, 0);
            if (!lua_isnoneornil(L, 5) && !cullingLuaVector(L, 5, offset)) {
                return cullingLuaFail("CullingGroup:setSphereTarget: offset must be a finite Vector3");
            }
            group->setSphereTarget(index, entity, static_cast<float>(radius), offset);
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // group:setSphere(index, BoundingSphere)
    int luaSetSphere(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "setSphere");
            uint32_t index = 0;
            if (!group || !cullingLuaIndex(L, 2, *group, "setSphere", index)) return false;
            BoundingSphere sphere;
            if (!cullingLuaSphere(L, 3, sphere) || !std::isfinite(sphere.radius) || sphere.radius < 0.0f) {
                return cullingLuaFail("CullingGroup:setSphere: argument 2 must be a BoundingSphere(center, radius >= 0)");
            }
            group->setSphere(index, sphere);
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // group:getSphere(index): a BoundingSphere
    int luaGetSphere(lua_State* L) {
        BoundingSphere sphere;
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "getSphere");
            uint32_t index = 0;
            if (!group || !cullingLuaIndex(L, 2, *group, "getSphere", index)) return false;
            sphere = group->getSphere(index);
            return true;
        }();
        if (!ok) return cullingLuaRaise(L);
        if (!luabridge::push(L, sphere)) lua_pushnil(L);
        return 1;
    }

    // group:setBoundingSpheres({BoundingSphere, ...})
    int luaSetBoundingSpheres(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "setBoundingSpheres");
            if (!group) return false;
            if (!lua_istable(L, 2)) return cullingLuaFail("CullingGroup:setBoundingSpheres: argument 1 must be a list of BoundingSphere");
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, 2));
            if (count > static_cast<lua_Integer>(CullingGroup::MAX_SPHERES)) {
                return cullingLuaFail("CullingGroup:setBoundingSpheres: %lld spheres; a group has at most %u", static_cast<long long>(count),
                                      CullingGroup::MAX_SPHERES);
            }
            std::vector<BoundingSphere> spheres(static_cast<size_t>(count));
            for (lua_Integer i = 1; i <= count; i++) {
                lua_rawgeti(L, 2, i);
                const bool valid = cullingLuaSphere(L, -1, spheres[static_cast<size_t>(i - 1)]);
                lua_pop(L, 1);
                if (!valid || !std::isfinite(spheres[static_cast<size_t>(i - 1)].radius) || spheres[static_cast<size_t>(i - 1)].radius < 0.0f) {
                    return cullingLuaFail("CullingGroup:setBoundingSpheres: element %lld is not a BoundingSphere(center, radius >= 0)", static_cast<long long>(i));
                }
            }
            group->setBoundingSpheres(spheres);
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // group:setBoundingDistances({d1, d2, ...})
    int luaSetBoundingDistances(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "setBoundingDistances");
            if (!group) return false;
            if (!lua_istable(L, 2)) return cullingLuaFail("CullingGroup:setBoundingDistances: argument 1 must be a list of distances");
            const size_t count = lua_rawlen(L, 2);
            if (count > CullingGroup::MAX_DISTANCES) {
                return cullingLuaFail("CullingGroup:setBoundingDistances: %zu distances; at most %zu", count, CullingGroup::MAX_DISTANCES);
            }
            float distances[CullingGroup::MAX_DISTANCES] = {};
            for (size_t i = 0; i < count; i++) {
                lua_rawgeti(L, 2, static_cast<lua_Integer>(i + 1));
                const bool number = lua_type(L, -1) == LUA_TNUMBER;
                distances[i] = number ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
                lua_pop(L, 1);
                if (!number || !std::isfinite(distances[i]) || distances[i] <= 0.0f || (i > 0 && distances[i] <= distances[i - 1])) {
                    return cullingLuaFail("CullingGroup:setBoundingDistances: distances must be finite, positive and ascending");
                }
            }
            group->setBoundingDistances(std::span<const float>(distances, count));
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // group:setDistanceReferencePoint(Vector3 | gameObject | entity)
    int luaSetDistanceReferencePoint(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "setDistanceReferencePoint");
            if (!group) return false;
            Vector3 point;
            Entity entity = NULL_ENTITY;
            if (cullingLuaVector(L, 2, point)) {
                group->setDistanceReferencePoint(point);
            } else if (cullingLuaEntity(L, 2, entity) && group->getScene() && group->getScene()->isEntityCreated(entity)) {
                group->setDistanceReferencePoint(entity);
            } else {
                return cullingLuaFail("CullingGroup:setDistanceReferencePoint: argument 1 must be a finite Vector3 or a GameObject (followed)");
            }
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // group:isVisible(index), group:getDistance(index), group:eraseSwapBack(index)
    template<int Kind>
    int luaIndexed(lua_State* L) {
        static const char* const names[] = {"isVisible", "getDistance", "eraseSwapBack"};
        int result = 0;
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, names[Kind]);
            uint32_t index = 0;
            if (!group || !cullingLuaIndex(L, 2, *group, names[Kind], index)) return false;
            if constexpr (Kind == 0) result = group->isVisible(index) ? 1 : 0;
            else if constexpr (Kind == 1) result = group->getDistance(index);
            else group->eraseSwapBack(index);
            return true;
        }();
        if (!ok) return cullingLuaRaise(L);
        if constexpr (Kind == 0) {
            lua_pushboolean(L, result);
            return 1;
        } else if constexpr (Kind == 1) {
            lua_pushinteger(L, result);
            return 1;
        } else {
            return 0;
        }
    }

    // group:queryIndices(visible, out [, first]), (band, out [, first]), (visible, band, out [, first]):
    // the matching sphere indices written into out[1..n] (its other entries are left); n
    int luaQueryIndices(lua_State* L) {
        static std::vector<uint32_t> scratch;
        uint32_t written = 0;
        int outArg = 0;
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "queryIndices");
            if (!group) return false;
            int form = 0;
            bool visible = false;
            int band = 0;
            if (lua_isboolean(L, 2) && lua_isinteger(L, 3)) {
                form = 2;
                visible = lua_toboolean(L, 2) != 0;
                band = static_cast<int>(lua_tointeger(L, 3));
                outArg = 4;
            } else if (lua_isboolean(L, 2)) {
                form = 0;
                visible = lua_toboolean(L, 2) != 0;
                outArg = 3;
            } else if (lua_isinteger(L, 2)) {
                form = 1;
                band = static_cast<int>(lua_tointeger(L, 2));
                outArg = 3;
            } else {
                return cullingLuaFail("CullingGroup:queryIndices: (visible, out [, first]), (band, out [, first]) or (visible, band, out [, first])");
            }
            if (!lua_istable(L, outArg)) return cullingLuaFail("CullingGroup:queryIndices: the out argument must be a table (it is filled from 1)");
            const lua_Integer first = luaL_optinteger(L, outArg + 1, 0);
            if (first < 0) return cullingLuaFail("CullingGroup:queryIndices: first must be >= 0");
            const uint32_t count = group->getBoundingSphereCount();
            if (scratch.size() < count) scratch.resize(count);
            const std::span<uint32_t> out(scratch.data(), count);
            const uint32_t skip = static_cast<uint32_t>(first);
            written = form == 0 ? group->queryIndices(visible, out, skip)
                    : (form == 1 ? group->queryIndices(band, out, skip) : group->queryIndices(visible, band, out, skip));
            return true;
        }();
        if (!ok) return cullingLuaRaise(L);
        for (uint32_t i = 0; i < written; i++) {
            lua_pushinteger(L, static_cast<lua_Integer>(scratch[i]) + 1);
            lua_rawseti(L, outArg, static_cast<lua_Integer>(i) + 1);
        }
        lua_pushinteger(L, written);
        return 1;
    }

    luabridge::LuaRef luaGetTargetCamera(const CullingGroup* group, lua_State* L) {
        const Entity camera = group->getTargetCamera();
        if (camera == NULL_ENTITY) return luabridge::LuaRef(L);
        return luabridge::LuaRef(L, static_cast<lua_Integer>(camera));
    }

    int luaSetTargetCamera(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "targetCamera");
            if (!group) return false;
            Entity camera = NULL_ENTITY;
            if (!lua_isnoneornil(L, 2) && !cullingLuaEntity(L, 2, camera)) {
                return cullingLuaFail("CullingGroup.targetCamera: a Camera GameObject (or its entity id), or nil for any enabled camera");
            }
            group->setTargetCamera(camera);
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    luabridge::LuaRef luaGetOnStateChanged(const CullingGroup*, lua_State* L) {
        return luabridge::LuaRef(L);
    }

    int luaSetOnStateChanged(lua_State* L) {
        const bool ok = [&]() -> bool {
            CullingGroup* group = cullingLuaSelf(L, "onStateChanged");
            if (!group) return false;
            if (!group->isValid()) return cullingLuaFail("CullingGroup.onStateChanged: the group was disposed");
            CullingSystem* system = group->getScene()->getSystem<CullingSystem>().get();
            if (lua_isnil(L, 2)) {
                system->setLuaCallback(group->getSlot(), group->getGeneration(), nullptr);
                return true;
            }
            if (!lua_isfunction(L, 2)) return cullingLuaFail("CullingGroup.onStateChanged: a function(index, isVisible, wasVisible, band, previousBand) or nil");
            lua_pushvalue(L, 2);
            auto callback = std::make_shared<LuaFunction<void>>(L);
            system->setLuaCallback(group->getSlot(), group->getGeneration(), std::move(callback));
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

    // ---- SimulationCulling

    // SimulationCulling.addFocus(scene, Vector3 | gameObject | entity): its id
    int luaAddFocus(lua_State* L) {
        int id = 0;
        const bool ok = [&]() -> bool {
            Scene* scene = cullingLuaScene(L, 1);
            if (!scene) return cullingLuaFail("SimulationCulling.addFocus: argument 1 must be a Scene (self.scene)");
            Vector3 position;
            Entity entity = NULL_ENTITY;
            if (cullingLuaVector(L, 2, position)) {
                id = SimulationCulling::addFocus(scene, position);
            } else if (cullingLuaEntity(L, 2, entity) && scene->isEntityCreated(entity)) {
                id = SimulationCulling::addFocus(scene, entity);
            } else {
                return cullingLuaFail("SimulationCulling.addFocus: argument 2 must be a finite Vector3 or a GameObject of the scene");
            }
            if (id == 0) {
                return cullingLuaFail("SimulationCulling.addFocus: a scene has at most %d script focuses; removeFocus one", SimulationCulling::MAX_SCRIPT_FOCUSES);
            }
            return true;
        }();
        if (!ok) return cullingLuaRaise(L);
        lua_pushinteger(L, id);
        return 1;
    }

    // SimulationCulling.setGroundCheck(scene, function(x, y, z, radius) return loaded end | nil)
    int luaSetGroundCheck(lua_State* L) {
        const bool ok = [&]() -> bool {
            Scene* scene = cullingLuaScene(L, 1);
            if (!scene) return cullingLuaFail("SimulationCulling.setGroundCheck: argument 1 must be a Scene (self.scene)");
            if (lua_isnoneornil(L, 2)) {
                SimulationCulling::setGroundCheck(scene, nullptr);
                return true;
            }
            if (!lua_isfunction(L, 2)) return cullingLuaFail("SimulationCulling.setGroundCheck: a function(x, y, z, radius) returning whether the ground is loaded, or nil");
            lua_pushvalue(L, 2);
            auto check = std::make_shared<LuaFunction<bool>>(L);
            SimulationCulling::setGroundCheck(scene, [check](const Vector3& position, float radius) {
                return (*check)(position.x, position.y, position.z, radius);
            });
            return true;
        }();
        return ok ? 0 : cullingLuaRaise(L);
    }

}
#endif

void LuaBinding::registerCullingClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // A sphere a CullingGroup tests: its centre and radius (metres)
    luabridge::getGlobalNamespace(L)
        .beginClass<BoundingSphere>("BoundingSphere")
        .addConstructor<void (*) (), void (*) (const Vector3&, float)>()
        .addProperty("position", &BoundingSphere::position)
        .addProperty("radius", &BoundingSphere::radius)
        .endClass();

    // Bounding spheres sorted into distance bands from a reference point and tested against the
    // frustum of one camera or of any enabled camera, once a frame while the scene runs (Unity's
    // CullingGroup; frustum only, no occlusion). Indices are 1-based, bands 0-based (0 nearest,
    // -1 before the first evaluation). Disposed with dispose(), or when the script that made it
    // is released; a group kept in a local keeps working
    luabridge::getGlobalNamespace(L)
        .beginClass<CullingGroup>("CullingGroup")
        .addConstructor(sceneconstructors::ofScene<CullingGroup>())
        // Ends the group: isValid() is false from then on
        .addFunction("dispose", &CullingGroup::dispose)
        .addFunction("isValid", &CullingGroup::isValid)
        .addProperty("enabled", &CullingGroup::isEnabled, &CullingGroup::setEnabled)
        // A Camera GameObject's id; nil: visible in any enabled camera of the scene
        .addProperty("targetCamera", &luaGetTargetCamera, &luaSetTargetCamera)
        .addProperty("bandHysteresis", &CullingGroup::getBandHysteresis, &CullingGroup::setBandHysteresis)
        // function(index, isVisible, wasVisible, band, previousBand): each sphere whose visibility or
        // band changed, after the group was evaluated; the first evaluation reports every sphere
        // (previousBand -1). Write-only (reads nil)
        .addProperty("onStateChanged", &luaGetOnStateChanged, &luaSetOnStateChanged)
        .addProperty("boundingSphereCount", &CullingGroup::getBoundingSphereCount)
        // Uses the first `count` spheres: new ones are at the origin with radius 0 until set
        .addFunction("setBoundingSphereCount", +[](CullingGroup* self, uint32_t count) { self->setBoundingSphereCount(count); })
        .addFunction("setBoundingSpheres", &luaSetBoundingSpheres)
        .addFunction("setSphere", &luaSetSphere)
        .addFunction("getSphere", &luaGetSphere)
        .addFunction("setSphereTarget", &luaSetSphereTarget)
        .addFunction("eraseSwapBack", &luaIndexed<2>)
        .addFunction("setBoundingDistances", &luaSetBoundingDistances)
        .addFunction("setDistanceReferencePoint", &luaSetDistanceReferencePoint)
        .addFunction("isVisible", &luaIndexed<0>)
        .addFunction("getDistance", &luaIndexed<1>)
        .addFunction("queryIndices", &luaQueryIndices)
        .endClass();

    // The Simulation Culling of a GameObject (ADDS one when it has none): beyond Simulation
    // Distance from every focus, or over ground that is not loaded, its scripts' update callbacks
    // are skipped, its Animator stops and its dynamic bodies freeze in place; between Reduced Rate
    // Distance and Simulation Distance onUpdate and onPostUpdate run every Reduced Update Interval
    // frames. Its scripts receive onSimulationCulled(reason) and onSimulationResumed()
    luabridge::getGlobalNamespace(L)
        .deriveClass<SimulationCulling, EntityHandle>("SimulationCulling")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &SimulationCulling::isEnabled, &SimulationCulling::setEnabled)
        .addProperty("simulationDistance", &SimulationCulling::getSimulationDistance, &SimulationCulling::setSimulationDistance)
        // 0: no reduced band
        .addProperty("reducedRateDistance", &SimulationCulling::getReducedRateDistance, &SimulationCulling::setReducedRateDistance)
        // frames, 2..60
        .addProperty("reducedUpdateInterval", &SimulationCulling::getReducedUpdateInterval, &SimulationCulling::setReducedUpdateInterval)
        .addProperty("hysteresis", &SimulationCulling::getHysteresis, &SimulationCulling::setHysteresis)
        // 0: the rendered or collider bounds when it starts, else 0.5
        .addProperty("boundsRadius", &SimulationCulling::getBoundsRadius, &SimulationCulling::setBoundsRadius)
        .addProperty("simulateWhenVisible", &SimulationCulling::isSimulateWhenVisible, &SimulationCulling::setSimulateWhenVisible)
        .addProperty("freezeWhenGroundNotLoaded", &SimulationCulling::isFreezeWhenGroundNotLoaded, &SimulationCulling::setFreezeWhenGroundNotLoaded)
        .addProperty("cullScripts", &SimulationCulling::isCullScripts, &SimulationCulling::setCullScripts)
        .addProperty("cullAnimator", &SimulationCulling::isCullAnimator, &SimulationCulling::setCullAnimator)
        .addProperty("cullPhysics", &SimulationCulling::isCullPhysics, &SimulationCulling::setCullPhysics)
        // "full", "reduced" or "culled"
        .addProperty("state", +[](const SimulationCulling* self) -> std::string { return SimulationCulling::stateName(self->getState()); })
        // "none", "distance" or "ground_not_loaded"
        .addProperty("reason", +[](const SimulationCulling* self) -> std::string { return SimulationCulling::reasonName(self->getReason()); })
        // metres to the nearest focus, from the bounds' surface
        .addProperty("focusDistance", &SimulationCulling::getFocusDistance)
        // A simulation focus at a position or following a GameObject (one per player); its id. Pair
        // it with TerrainWorld:addFocus: a simulation focus streams no terrain
        .addStaticFunction("addFocus", &luaAddFocus)
        // Moves a focus to a position (it stops following); false when no focus has that id
        .addStaticFunction("setFocus", +[](Scene* scene, int id, const Vector3& position) -> bool { return SimulationCulling::setFocus(scene, id, position); })
        .addStaticFunction("removeFocus", +[](Scene* scene, int id) -> bool { return SimulationCulling::removeFocus(scene, id); })
        // With script focuses, whether the main camera is a focus too (default true)
        .addStaticFunction("setCameraIsFocus", +[](Scene* scene, bool cameraIsFocus) { SimulationCulling::setCameraIsFocus(scene, cameraIsFocus); })
        .addStaticFunction("isCameraFocus", +[](Scene* scene) -> bool { return SimulationCulling::isCameraFocus(scene); })
        // Multiplies every component's distances, on top of the Device Tier's Simulation Distance
        .addStaticProperty("distanceScale", &SimulationCulling::getDistanceScale, &SimulationCulling::setDistanceScale)
        // function(x, y, z, radius) returning whether the ground there is loaded (worlds a game
        // streams itself), or nil to remove it; called within simulation distance, 256 a frame at most
        .addStaticFunction("setGroundCheck", &luaSetGroundCheck)
        .endClass();

#endif
}
