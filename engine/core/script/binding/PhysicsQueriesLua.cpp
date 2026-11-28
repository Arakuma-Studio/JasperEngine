// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Unity's shape queries in Lua (docs/specs/S96-physics-gaps.md REQ-10..REQ-13, D4): the casts,
// overlaps and checks of Physics and Physics2D, Rigidbody:sweepTest, Rigidbody2D:cast, the
// options table and RayReturn's collider / gameObject / rigidbody / transform. Plain
// lua_CFunctions (Physics.raycast's pattern), no luabridge::overload.

#include "script/binding/PhysicsQueriesLua.h"

#include "LuaBinding.h"
#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "object/EntityHandle.h"
#include "object/GameObject.h"
#include "object/physics/Collision.h"
#include "object/physics/Collision2D.h"
#include "registry/PhysicsLayers.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#include "object/physics/Rigidbody.h"
#include "physics/PhysicsQueries3D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#include "object/physics/Rigidbody2D.h"
#include "physics/PhysicsQueries2D.h"
#endif

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace jasperengine::luaphysics {

    void readQueryOptions(lua_State* L, int index, const char* function, bool is2D, QueryOptions& options, std::vector<Entity>& ignore){
        if (lua_isnoneornil(L, index)){
            return;
        }
        if (!lua_istable(L, index)){
            luaL_error(L, "%s: options must be a table ({ignore, triggers, %s})", function, is2D ? "startInColliders" : "hitFromInside, hitBackfaces");
            return;
        }
        index = lua_absindex(L, index);

        lua_getfield(L, index, "ignore");
        if (lua_istable(L, -1)){
            const lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= count; i++){
                lua_rawgeti(L, -1, i);
                Entity entity = NULL_ENTITY;
                if (lua_isinteger(L, -1)){
                    entity = (Entity)lua_tointeger(L, -1);
                }else if (luabridge::isInstance<EntityHandle>(L, -1)){
                    auto handle = luabridge::get<EntityHandle*>(L, -1);
                    if (handle && handle.value()){
                        entity = handle.value()->getEntity();
                    }
                }else{
                    lua_pop(L, 2);
                    luaL_error(L, "%s: options.ignore[%d] must be a GameObject, a Rigidbody or an entity", function, (int)i);
                    return;
                }
                ignore.push_back(entity);
                lua_pop(L, 1);
            }
        }else if (!lua_isnil(L, -1)){
            lua_pop(L, 1);
            luaL_error(L, "%s: options.ignore must be a table of GameObjects, Rigidbodies or entities", function);
            return;
        }
        lua_pop(L, 1);
        options.ignore = ignore.empty() ? nullptr : &ignore;

        lua_getfield(L, index, "triggers");
        if (!lua_isnil(L, -1)){
            const lua_Integer value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -1;
            if (value < (lua_Integer)QueryTriggerInteraction::USE_GLOBAL || value > (lua_Integer)QueryTriggerInteraction::COLLIDE){
                lua_pop(L, 1);
                luaL_error(L, "%s: options.triggers must be QueryTriggerInteraction.USE_GLOBAL, IGNORE or COLLIDE", function);
                return;
            }
            options.triggers = (QueryTriggerInteraction)value;
        }
        lua_pop(L, 1);

        if (is2D){
            lua_getfield(L, index, "startInColliders");
            if (!lua_isnil(L, -1)) options.startInColliders = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }else{
            lua_getfield(L, index, "hitFromInside");
            if (!lua_isnil(L, -1)) options.hitFromInside = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, index, "hitBackfaces");
            if (!lua_isnil(L, -1)) options.hitBackfaces = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }

        lua_getfield(L, index, "maxHits");
        if (!lua_isnil(L, -1)){
            const lua_Integer value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
            if (value < 1){
                lua_pop(L, 1);
                luaL_error(L, "%s: options.maxHits must be a positive integer", function);
                return;
            }
            options.maxHits = (size_t)value;
        }
        lua_pop(L, 1);
    }

}

namespace jasperengine::pqlua {

    Scene* sceneArg(lua_State* L, const char* function){
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result && result.value()){
                return result.value();
            }
        }
        luaL_error(L, "%s: argument 1 (scene) must be a Scene", function);
        return nullptr;
    }

    template<typename T>
    T valueArg(lua_State* L, int index, const char* function, const char* name, const char* type){
        if (luabridge::isInstance<T>(L, index)){
            auto result = luabridge::get<T>(L, index);
            if (result){
                return result.value();
            }
        }
        luaL_error(L, "%s: argument %d (%s) must be a %s", function, index, name, type);
        return T();
    }

    // a bound handle (Rigidbody, Rigidbody2D) at `index`, never null (raises instead)
    template<typename T>
    T* handleArg(lua_State* L, int index, const char* function, const char* type){
        if (luabridge::isInstance<T>(L, index)){
            auto result = luabridge::get<T*>(L, index);
            if (result && result.value()){
                return result.value();
            }
        }
        luaL_error(L, "%s: call it with ':' on a %s", function, type);
        return nullptr;
    }

    Vector3 vec3Arg(lua_State* L, int index, const char* function, const char* name){
        return valueArg<Vector3>(L, index, function, name, "Vector3");
    }

    Vector2 vec2Arg(lua_State* L, int index, const char* function, const char* name){
        return valueArg<Vector2>(L, index, function, name, "Vector2");
    }

    // nil is Quaternion.IDENTITY (Unity's default orientation)
    Quaternion rotationArg(lua_State* L, int index, const char* function, const char* name){
        if (lua_isnoneornil(L, index)){
            return Quaternion::IDENTITY;
        }
        return valueArg<Quaternion>(L, index, function, name, "Quaternion");
    }

    float numberArg(lua_State* L, int index, const char* function, const char* name){
        if (!lua_isnumber(L, index)){
            luaL_error(L, "%s: argument %d (%s) must be a number", function, index, name);
        }
        return (float)lua_tonumber(L, index);
    }

    CapsuleDirection2D capsuleDirectionArg(lua_State* L, int index, const char* function){
        const lua_Integer value = lua_isinteger(L, index) ? lua_tointeger(L, index) : -1;
        if (value != (lua_Integer)CapsuleDirection2D::VERTICAL && value != (lua_Integer)CapsuleDirection2D::HORIZONTAL){
            luaL_error(L, "%s: argument %d (capsuleDirection) must be CapsuleDirection2D.VERTICAL or HORIZONTAL", function, index);
        }
        return (CapsuleDirection2D)value;
    }

    void check(lua_State* L, const char* function, const char* argument, const char* why){
        if (why){
            luaL_error(L, "%s: %s (%s)", function, why, argument);
        }
    }

    // [layerMask [, options]] from `index`
    void maskAndOptions(lua_State* L, int index, const char* function, bool is2D, QueryOptions& options, std::vector<Entity>& ignore){
        if (!lua_isnoneornil(L, index)){
            if (!lua_isinteger(L, index)){
                luaL_error(L, "%s: argument %d (layerMask) must be an integer", function, index);
            }
            options.layerMask = (uint32_t)lua_tointeger(L, index);
        }
        luaphysics::readQueryOptions(L, index + 1, function, is2D, options, ignore);
    }

    // a hit, or nil for none (casts: Unity returns false)
    int pushHit(lua_State* L, const RayReturn& hit){
        if (!hit.hit || !luabridge::push(L, hit)){
            lua_pushnil(L);
        }
        return 1;
    }

    int pushHits(lua_State* L, const std::vector<RayReturn>& hits){
        lua_createtable(L, (int)hits.size(), 0);
        for (size_t i = 0; i < hits.size(); i++){
            if (!luabridge::push(L, hits[i])){
                lua_pushnil(L);
            }
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        return 1;
    }

    template<typename Ref>
    int pushColliders(lua_State* L, const std::vector<Ref>& colliders){
        lua_createtable(L, (int)colliders.size(), 0);
        for (size_t i = 0; i < colliders.size(); i++){
            if (!luabridge::push(L, colliders[i])){
                lua_pushnil(L);
            }
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        return 1;
    }

    // RayReturn (RaycastHit) members resolved through the hit's scene; nil without one
    luabridge::LuaRef hitCollider(const RayReturn* self, lua_State* L){
        if (!self->hit || !self->scene || !self->scene->isEntityCreated(self->body)){
            return luabridge::LuaRef(L);
        }
#ifdef JASPERENGINE_PHYSICS_2D
        if (self->body2D && self->scene->findComponent<Rigidbody2DComponent>(self->body)){
            return luabridge::LuaRef(L, ColliderRef2D(self->scene, self->body, (int)self->colliderIndex));
        }
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        if (!self->body2D && self->scene->findComponent<RigidbodyComponent>(self->body)){
            return luabridge::LuaRef(L, ColliderRef(self->scene, self->body, (int)self->colliderIndex));
        }
#endif
        return luabridge::LuaRef(L);
    }

    luabridge::LuaRef hitGameObject(const RayReturn* self, lua_State* L){
        if (!self->hit || !self->scene || !self->scene->isEntityCreated(self->body)){
            return luabridge::LuaRef(L);
        }
        return luabridge::LuaRef(L, GameObject(self->scene, self->body));
    }

    luabridge::LuaRef hitRigidbody(const RayReturn* self, lua_State* L){
        if (!self->hit || !self->scene || !self->scene->isEntityCreated(self->body)){
            return luabridge::LuaRef(L);
        }
#ifdef JASPERENGINE_PHYSICS_2D
        if (self->body2D && self->scene->findComponent<Rigidbody2DComponent>(self->body)){
            return luabridge::LuaRef(L, Rigidbody2D(self->scene, self->body));
        }
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        if (!self->body2D && self->scene->findComponent<RigidbodyComponent>(self->body)){
            return luabridge::LuaRef(L, Rigidbody(self->scene, self->body));
        }
#endif
        return luabridge::LuaRef(L);
    }

#ifdef JASPERENGINE_PHYSICS_3D
    // ---- Physics (3D)

    // sphereCast(scene, origin, radius, direction, maxDistance [, layerMask [, options]])
    template<bool all>
    int sphereCast(lua_State* L){
        const char* function = all ? "Physics.sphereCastAll" : "Physics.sphereCast";
        Scene* scene = sceneArg(L, function);
        const Vector3 origin = vec3Arg(L, 2, function, "origin");
        const float radius = numberArg(L, 3, function, "radius");
        const Vector3 direction = vec3Arg(L, 4, function, "direction");
        const float maxDistance = numberArg(L, 5, function, "maxDistance");
        check(L, function, "origin", PhysicsQueries3D::invalidPoint(origin));
        check(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
        check(L, function, "direction, maxDistance", PhysicsQueries3D::invalidDirection(direction, maxDistance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 6, function, false, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries3D::sphereCastAll(scene, origin, radius, direction, maxDistance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries3D::sphereCast(scene, origin, radius, direction, maxDistance, options));
    }

    // boxCast(scene, center, halfExtents, direction, orientation, maxDistance [, layerMask [, options]])
    template<bool all>
    int boxCast(lua_State* L){
        const char* function = all ? "Physics.boxCastAll" : "Physics.boxCast";
        Scene* scene = sceneArg(L, function);
        const Vector3 center = vec3Arg(L, 2, function, "center");
        const Vector3 halfExtents = vec3Arg(L, 3, function, "halfExtents");
        const Vector3 direction = vec3Arg(L, 4, function, "direction");
        const Quaternion orientation = rotationArg(L, 5, function, "orientation");
        const float maxDistance = numberArg(L, 6, function, "maxDistance");
        check(L, function, "center", PhysicsQueries3D::invalidPoint(center));
        check(L, function, "halfExtents", PhysicsQueries3D::invalidHalfExtents(halfExtents));
        check(L, function, "direction, maxDistance", PhysicsQueries3D::invalidDirection(direction, maxDistance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 7, function, false, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries3D::boxCastAll(scene, center, halfExtents, direction, orientation, maxDistance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries3D::boxCast(scene, center, halfExtents, direction, orientation, maxDistance, options));
    }

    // capsuleCast(scene, point1, point2, radius, direction, maxDistance [, layerMask [, options]])
    template<bool all>
    int capsuleCast(lua_State* L){
        const char* function = all ? "Physics.capsuleCastAll" : "Physics.capsuleCast";
        Scene* scene = sceneArg(L, function);
        const Vector3 point1 = vec3Arg(L, 2, function, "point1");
        const Vector3 point2 = vec3Arg(L, 3, function, "point2");
        const float radius = numberArg(L, 4, function, "radius");
        const Vector3 direction = vec3Arg(L, 5, function, "direction");
        const float maxDistance = numberArg(L, 6, function, "maxDistance");
        check(L, function, "point1", PhysicsQueries3D::invalidPoint(point1));
        check(L, function, "point2", PhysicsQueries3D::invalidPoint(point2));
        check(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
        check(L, function, "direction, maxDistance", PhysicsQueries3D::invalidDirection(direction, maxDistance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 7, function, false, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries3D::capsuleCastAll(scene, point1, point2, radius, direction, maxDistance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries3D::capsuleCast(scene, point1, point2, radius, direction, maxDistance, options));
    }

    // overlapSphere / checkSphere(scene, center, radius [, layerMask [, options]])
    template<bool checkOnly>
    int sphereOverlap(lua_State* L){
        const char* function = checkOnly ? "Physics.checkSphere" : "Physics.overlapSphere";
        Scene* scene = sceneArg(L, function);
        const Vector3 center = vec3Arg(L, 2, function, "center");
        const float radius = numberArg(L, 3, function, "radius");
        check(L, function, "center", PhysicsQueries3D::invalidPoint(center));
        check(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 4, function, false, options, ignore);
        if (checkOnly){
            lua_pushboolean(L, PhysicsQueries3D::checkSphere(scene, center, radius, options) ? 1 : 0);
            return 1;
        }
        std::vector<ColliderRef> colliders;
        PhysicsQueries3D::overlapSphere(scene, center, radius, colliders, options);
        return pushColliders(L, colliders);
    }

    // overlapBox / checkBox(scene, center, halfExtents [, orientation [, layerMask [, options]]])
    template<bool checkOnly>
    int boxOverlap(lua_State* L){
        const char* function = checkOnly ? "Physics.checkBox" : "Physics.overlapBox";
        Scene* scene = sceneArg(L, function);
        const Vector3 center = vec3Arg(L, 2, function, "center");
        const Vector3 halfExtents = vec3Arg(L, 3, function, "halfExtents");
        const Quaternion orientation = rotationArg(L, 4, function, "orientation");
        check(L, function, "center", PhysicsQueries3D::invalidPoint(center));
        check(L, function, "halfExtents", PhysicsQueries3D::invalidHalfExtents(halfExtents));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 5, function, false, options, ignore);
        if (checkOnly){
            lua_pushboolean(L, PhysicsQueries3D::checkBox(scene, center, halfExtents, orientation, options) ? 1 : 0);
            return 1;
        }
        std::vector<ColliderRef> colliders;
        PhysicsQueries3D::overlapBox(scene, center, halfExtents, orientation, colliders, options);
        return pushColliders(L, colliders);
    }

    // overlapCapsule / checkCapsule(scene, point1, point2, radius [, layerMask [, options]])
    template<bool checkOnly>
    int capsuleOverlap(lua_State* L){
        const char* function = checkOnly ? "Physics.checkCapsule" : "Physics.overlapCapsule";
        Scene* scene = sceneArg(L, function);
        const Vector3 point1 = vec3Arg(L, 2, function, "point1");
        const Vector3 point2 = vec3Arg(L, 3, function, "point2");
        const float radius = numberArg(L, 4, function, "radius");
        check(L, function, "point1", PhysicsQueries3D::invalidPoint(point1));
        check(L, function, "point2", PhysicsQueries3D::invalidPoint(point2));
        check(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 5, function, false, options, ignore);
        if (checkOnly){
            lua_pushboolean(L, PhysicsQueries3D::checkCapsule(scene, point1, point2, radius, options) ? 1 : 0);
            return 1;
        }
        std::vector<ColliderRef> colliders;
        PhysicsQueries3D::overlapCapsule(scene, point1, point2, radius, colliders, options);
        return pushColliders(L, colliders);
    }

    // closestPoint(scene, point, collider)
    int closestPoint(lua_State* L){
        const char* function = "Physics.closestPoint";
        Scene* scene = sceneArg(L, function);
        const Vector3 point = vec3Arg(L, 2, function, "point");
        const ColliderRef collider = valueArg<ColliderRef>(L, 3, function, "collider", "Collider");
        check(L, function, "point", PhysicsQueries3D::invalidPoint(point));
        Vector3 closest;
        if (!PhysicsQueries3D::closestPoint(scene, point, collider, closest)){
            return luaL_error(L, "%s: the collider must be a live convex collider (sphere, box, capsule, cylinder or convex hull) "
                                 "of a built body; for a mesh collider use Physics.raycast towards it", function);
        }
        if (!luabridge::push(L, closest)){
            lua_pushnil(L);
        }
        return 1;
    }

    // rigidbody:sweepTest(direction, maxDistance [, options]) / sweepTestAll
    template<bool all>
    int sweepTest(lua_State* L){
        const char* function = all ? "Rigidbody.sweepTestAll" : "Rigidbody.sweepTest";
        const Rigidbody& body = *handleArg<Rigidbody>(L, 1, function, "Rigidbody");
        const Vector3 direction = vec3Arg(L, 2, function, "direction");
        const float maxDistance = numberArg(L, 3, function, "maxDistance");
        check(L, function, "direction, maxDistance", PhysicsQueries3D::invalidDirection(direction, maxDistance));
        QueryOptions options;
        std::vector<Entity> ignore;
        luaphysics::readQueryOptions(L, 4, function, false, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries3D::sweepTestAll(body.getScene(), body.getEntity(), direction, maxDistance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries3D::sweepTest(body.getScene(), body.getEntity(), direction, maxDistance, options));
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    // ---- Physics2D

    // circleCast(scene, origin, radius, direction, distance [, layerMask [, options]])
    template<bool all>
    int circleCast(lua_State* L){
        const char* function = all ? "Physics2D.circleCastAll" : "Physics2D.circleCast";
        Scene* scene = sceneArg(L, function);
        const Vector2 origin = vec2Arg(L, 2, function, "origin");
        const float radius = numberArg(L, 3, function, "radius");
        const Vector2 direction = vec2Arg(L, 4, function, "direction");
        const float distance = numberArg(L, 5, function, "distance");
        check(L, function, "origin", PhysicsQueries2D::invalidPoint(origin));
        check(L, function, "radius", PhysicsQueries2D::invalidRadius(radius));
        check(L, function, "direction, distance", PhysicsQueries2D::invalidDirection(direction, distance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 6, function, true, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries2D::circleCastAll(scene, origin, radius, direction, distance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries2D::circleCast(scene, origin, radius, direction, distance, options));
    }

    // boxCast(scene, origin, size, angle, direction, distance [, layerMask [, options]])
    template<bool all>
    int boxCast2D(lua_State* L){
        const char* function = all ? "Physics2D.boxCastAll" : "Physics2D.boxCast";
        Scene* scene = sceneArg(L, function);
        const Vector2 origin = vec2Arg(L, 2, function, "origin");
        const Vector2 size = vec2Arg(L, 3, function, "size");
        const float angle = numberArg(L, 4, function, "angle");
        const Vector2 direction = vec2Arg(L, 5, function, "direction");
        const float distance = numberArg(L, 6, function, "distance");
        check(L, function, "origin", PhysicsQueries2D::invalidPoint(origin));
        check(L, function, "size", PhysicsQueries2D::invalidSize(size));
        check(L, function, "direction, distance", PhysicsQueries2D::invalidDirection(direction, distance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 7, function, true, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries2D::boxCastAll(scene, origin, size, angle, direction, distance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries2D::boxCast(scene, origin, size, angle, direction, distance, options));
    }

    // capsuleCast(scene, origin, size, capsuleDirection, angle, direction, distance [, layerMask [, options]])
    template<bool all>
    int capsuleCast2D(lua_State* L){
        const char* function = all ? "Physics2D.capsuleCastAll" : "Physics2D.capsuleCast";
        Scene* scene = sceneArg(L, function);
        const Vector2 origin = vec2Arg(L, 2, function, "origin");
        const Vector2 size = vec2Arg(L, 3, function, "size");
        const CapsuleDirection2D capsuleDirection = capsuleDirectionArg(L, 4, function);
        const float angle = numberArg(L, 5, function, "angle");
        const Vector2 direction = vec2Arg(L, 6, function, "direction");
        const float distance = numberArg(L, 7, function, "distance");
        check(L, function, "origin", PhysicsQueries2D::invalidPoint(origin));
        check(L, function, "size", PhysicsQueries2D::invalidSize(size));
        check(L, function, "direction, distance", PhysicsQueries2D::invalidDirection(direction, distance));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 8, function, true, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries2D::capsuleCastAll(scene, origin, size, capsuleDirection, angle, direction, distance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries2D::capsuleCast(scene, origin, size, capsuleDirection, angle, direction, distance, options));
    }

    // the first collider (nil for none), or with `all` every one
    template<bool all, typename First, typename Every>
    int overlap2D(lua_State* L, const QueryOptions& options, First first, Every every){
        if (all){
            std::vector<ColliderRef2D> colliders;
            every(colliders, options);
            return pushColliders(L, colliders);
        }
        ColliderRef2D collider;
        if (!first(collider, options) || !luabridge::push(L, collider)){
            lua_pushnil(L);
        }
        return 1;
    }

    // overlapCircle(scene, point, radius [, layerMask [, options]])
    template<bool all>
    int overlapCircle(lua_State* L){
        const char* function = all ? "Physics2D.overlapCircleAll" : "Physics2D.overlapCircle";
        Scene* scene = sceneArg(L, function);
        const Vector2 point = vec2Arg(L, 2, function, "point");
        const float radius = numberArg(L, 3, function, "radius");
        check(L, function, "point", PhysicsQueries2D::invalidPoint(point));
        check(L, function, "radius", PhysicsQueries2D::invalidRadius(radius));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 4, function, true, options, ignore);
        return overlap2D<all>(L, options,
            [&](ColliderRef2D& out, const QueryOptions& o){ return PhysicsQueries2D::overlapCircle(scene, point, radius, out, o); },
            [&](std::vector<ColliderRef2D>& out, const QueryOptions& o){ return PhysicsQueries2D::overlapCircleAll(scene, point, radius, out, o); });
    }

    // overlapBox(scene, point, size, angle [, layerMask [, options]])
    template<bool all>
    int overlapBox2D(lua_State* L){
        const char* function = all ? "Physics2D.overlapBoxAll" : "Physics2D.overlapBox";
        Scene* scene = sceneArg(L, function);
        const Vector2 point = vec2Arg(L, 2, function, "point");
        const Vector2 size = vec2Arg(L, 3, function, "size");
        const float angle = numberArg(L, 4, function, "angle");
        check(L, function, "point", PhysicsQueries2D::invalidPoint(point));
        check(L, function, "size", PhysicsQueries2D::invalidSize(size));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 5, function, true, options, ignore);
        return overlap2D<all>(L, options,
            [&](ColliderRef2D& out, const QueryOptions& o){ return PhysicsQueries2D::overlapBox(scene, point, size, angle, out, o); },
            [&](std::vector<ColliderRef2D>& out, const QueryOptions& o){ return PhysicsQueries2D::overlapBoxAll(scene, point, size, angle, out, o); });
    }

    // overlapCapsule(scene, point, size, capsuleDirection, angle [, layerMask [, options]])
    template<bool all>
    int overlapCapsule2D(lua_State* L){
        const char* function = all ? "Physics2D.overlapCapsuleAll" : "Physics2D.overlapCapsule";
        Scene* scene = sceneArg(L, function);
        const Vector2 point = vec2Arg(L, 2, function, "point");
        const Vector2 size = vec2Arg(L, 3, function, "size");
        const CapsuleDirection2D capsuleDirection = capsuleDirectionArg(L, 4, function);
        const float angle = numberArg(L, 5, function, "angle");
        check(L, function, "point", PhysicsQueries2D::invalidPoint(point));
        check(L, function, "size", PhysicsQueries2D::invalidSize(size));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 6, function, true, options, ignore);
        return overlap2D<all>(L, options,
            [&](ColliderRef2D& out, const QueryOptions& o){ return PhysicsQueries2D::overlapCapsule(scene, point, size, capsuleDirection, angle, out, o); },
            [&](std::vector<ColliderRef2D>& out, const QueryOptions& o){ return PhysicsQueries2D::overlapCapsuleAll(scene, point, size, capsuleDirection, angle, out, o); });
    }

    // overlapPoint(scene, point [, layerMask [, options]])
    template<bool all>
    int overlapPoint(lua_State* L){
        const char* function = all ? "Physics2D.overlapPointAll" : "Physics2D.overlapPoint";
        Scene* scene = sceneArg(L, function);
        const Vector2 point = vec2Arg(L, 2, function, "point");
        check(L, function, "point", PhysicsQueries2D::invalidPoint(point));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 3, function, true, options, ignore);
        return overlap2D<all>(L, options,
            [&](ColliderRef2D& out, const QueryOptions& o){ return PhysicsQueries2D::overlapPoint(scene, point, out, o); },
            [&](std::vector<ColliderRef2D>& out, const QueryOptions& o){ return PhysicsQueries2D::overlapPointAll(scene, point, out, o); });
    }

    // overlapArea(scene, pointA, pointB [, layerMask [, options]])
    template<bool all>
    int overlapArea(lua_State* L){
        const char* function = all ? "Physics2D.overlapAreaAll" : "Physics2D.overlapArea";
        Scene* scene = sceneArg(L, function);
        const Vector2 pointA = vec2Arg(L, 2, function, "pointA");
        const Vector2 pointB = vec2Arg(L, 3, function, "pointB");
        check(L, function, "pointA", PhysicsQueries2D::invalidPoint(pointA));
        check(L, function, "pointB", PhysicsQueries2D::invalidPoint(pointB));
        check(L, function, "pointA, pointB", PhysicsQueries2D::invalidSize(Vector2(std::fabs(pointB.x - pointA.x), std::fabs(pointB.y - pointA.y))));
        QueryOptions options;
        std::vector<Entity> ignore;
        maskAndOptions(L, 4, function, true, options, ignore);
        return overlap2D<all>(L, options,
            [&](ColliderRef2D& out, const QueryOptions& o){ return PhysicsQueries2D::overlapArea(scene, pointA, pointB, out, o); },
            [&](std::vector<ColliderRef2D>& out, const QueryOptions& o){ return PhysicsQueries2D::overlapAreaAll(scene, pointA, pointB, out, o); });
    }

    // rigidbody2d:cast(direction, distance [, options]) / castAll
    template<bool all>
    int bodyCast2D(lua_State* L){
        const char* function = all ? "Rigidbody2D.castAll" : "Rigidbody2D.cast";
        const Rigidbody2D& body = *handleArg<Rigidbody2D>(L, 1, function, "Rigidbody2D");
        const Vector2 direction = vec2Arg(L, 2, function, "direction");
        const float distance = numberArg(L, 3, function, "distance");
        check(L, function, "direction, distance", PhysicsQueries2D::invalidDirection(direction, distance));
        QueryOptions options;
        std::vector<Entity> ignore;
        luaphysics::readQueryOptions(L, 4, function, true, options, ignore);
        if (all){
            std::vector<RayReturn> hits;
            PhysicsQueries2D::castAll(body.getScene(), body.getEntity(), direction, distance, hits, options);
            return pushHits(L, hits);
        }
        return pushHit(L, PhysicsQueries2D::cast(body.getScene(), body.getEntity(), direction, distance, options));
    }
#endif

}

void LuaBinding::registerPhysicsQueryClasses(lua_State* L){
    using namespace jasperengine::pqlua;

    luabridge::getGlobalNamespace(L)
        .beginNamespace("QueryTriggerInteraction")
        .addVariable("USE_GLOBAL", QueryTriggerInteraction::USE_GLOBAL)
        .addVariable("IGNORE", QueryTriggerInteraction::IGNORE)
        .addVariable("COLLIDE", QueryTriggerInteraction::COLLIDE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CapsuleDirection2D")
        .addVariable("VERTICAL", CapsuleDirection2D::VERTICAL)
        .addVariable("HORIZONTAL", CapsuleDirection2D::HORIZONTAL)
        .endNamespace();

    // Unity's RaycastHit fields (the class keeps its name; the stubs alias it RaycastHit)
    luabridge::getGlobalNamespace(L)
        .beginClass<RayReturn>("RayReturn")
        .addProperty("collider", &hitCollider)
        .addProperty("gameObject", &hitGameObject)
        .addProperty("rigidbody", &hitRigidbody)
        .addProperty("transform", &hitGameObject)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics>("Physics")
        .addStaticProperty("queriesHitTriggers", &Physics::getQueriesHitTriggers, &Physics::setQueriesHitTriggers)
        .addStaticProperty("queriesHitBackfaces", &Physics::getQueriesHitBackfaces, &Physics::setQueriesHitBackfaces)
#ifdef JASPERENGINE_PHYSICS_3D
        .addStaticFunction("sphereCast", &sphereCast<false>)
        .addStaticFunction("sphereCastAll", &sphereCast<true>)
        .addStaticFunction("boxCast", &boxCast<false>)
        .addStaticFunction("boxCastAll", &boxCast<true>)
        .addStaticFunction("capsuleCast", &capsuleCast<false>)
        .addStaticFunction("capsuleCastAll", &capsuleCast<true>)
        .addStaticFunction("overlapSphere", &sphereOverlap<false>)
        .addStaticFunction("overlapBox", &boxOverlap<false>)
        .addStaticFunction("overlapCapsule", &capsuleOverlap<false>)
        .addStaticFunction("checkSphere", &sphereOverlap<true>)
        .addStaticFunction("checkBox", &boxOverlap<true>)
        .addStaticFunction("checkCapsule", &capsuleOverlap<true>)
        .addStaticFunction("closestPoint", &closestPoint)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics2D>("Physics2D")
        .addStaticProperty("queriesHitTriggers", &Physics2D::getQueriesHitTriggers, &Physics2D::setQueriesHitTriggers)
        .addStaticProperty("queriesStartInColliders", &Physics2D::getQueriesStartInColliders, &Physics2D::setQueriesStartInColliders)
#ifdef JASPERENGINE_PHYSICS_2D
        .addStaticFunction("circleCast", &circleCast<false>)
        .addStaticFunction("circleCastAll", &circleCast<true>)
        .addStaticFunction("boxCast", &boxCast2D<false>)
        .addStaticFunction("boxCastAll", &boxCast2D<true>)
        .addStaticFunction("capsuleCast", &capsuleCast2D<false>)
        .addStaticFunction("capsuleCastAll", &capsuleCast2D<true>)
        .addStaticFunction("overlapCircle", &overlapCircle<false>)
        .addStaticFunction("overlapCircleAll", &overlapCircle<true>)
        .addStaticFunction("overlapBox", &overlapBox2D<false>)
        .addStaticFunction("overlapBoxAll", &overlapBox2D<true>)
        .addStaticFunction("overlapCapsule", &overlapCapsule2D<false>)
        .addStaticFunction("overlapCapsuleAll", &overlapCapsule2D<true>)
        .addStaticFunction("overlapPoint", &overlapPoint<false>)
        .addStaticFunction("overlapPointAll", &overlapPoint<true>)
        .addStaticFunction("overlapArea", &overlapArea<false>)
        .addStaticFunction("overlapAreaAll", &overlapArea<true>)
#endif
        .endClass();

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginClass<Rigidbody>("Rigidbody")
        .addFunction("sweepTest", &sweepTest<false>)
        .addFunction("sweepTestAll", &sweepTest<true>)
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginClass<Rigidbody2D>("Rigidbody2D")
        .addFunction("cast", &bodyCast2D<false>)
        .addFunction("castAll", &bodyCast2D<true>)
        .endClass();
#endif
}
