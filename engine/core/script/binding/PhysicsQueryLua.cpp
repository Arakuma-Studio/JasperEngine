// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The spatial queries in Lua (docs/specs/spatial-queries.md R28, D9): Physics and Physics2D
// overlaps, checks and shape casts, MeshRaycast's bounds overlaps, QueryTriggerInteraction,
// CapsuleDirection2D, OverlapHit and MeshOverlapHit. Reopens the Physics, Physics2D and
// MeshRaycast classes CoreClassesLua.cpp registers.
//
// The ...NonAlloc forms write entity ids (integers: no userdata, no garbage) into the caller's
// table and clear its entries past the count, so #results is the count. The results go through
// one main-thread buffer: a query with a table that has room allocates nothing.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so every argument is read before a
// local with a destructor exists.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Scene.h"
#include "object/EntityHandle.h"
#include "picking/MeshRaycast.h"
#include "physics/PhysicsQueryReference.h"
#include "registry/PhysicsLayers.h"

#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    // the main thread's buffers (the Lua state belongs to it)
    std::vector<OverlapHit>& pqOverlapBuffer(){
        static std::vector<OverlapHit> hits;
        return hits;
    }

    std::vector<RayReturn>& pqCastBuffer(){
        static std::vector<RayReturn> hits;
        return hits;
    }

    std::vector<MeshOverlapHit>& pqMeshBuffer(){
        static std::vector<MeshOverlapHit> hits;
        return hits;
    }

    std::vector<Entity>& pqIgnoreBuffer(){
        static std::vector<Entity> entities;
        return entities;
    }

    Scene* pqScene(lua_State* L, const char* function){
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result && result.value()){
                return result.value();
            }
        }
        luaL_error(L, "%s: argument 1 must be a Scene", function);
        return nullptr;
    }

    Vector3 pqVector3(lua_State* L, int index, const char* function, const char* name){
        if (luabridge::isInstance<Vector3>(L, index)){
            auto result = luabridge::get<Vector3>(L, index);
            if (result){
                return result.value();
            }
        }
        luaL_error(L, "%s: %s (argument %d) must be a Vector3", function, name, index);
        return Vector3::ZERO;
    }

    Quaternion pqOptRotation(lua_State* L, int index, const char* function){
        if (lua_isnoneornil(L, index)){
            return Quaternion::IDENTITY;
        }
        if (luabridge::isInstance<Quaternion>(L, index)){
            auto result = luabridge::get<Quaternion>(L, index);
            if (result){
                return result.value();
            }
        }
        luaL_error(L, "%s: rotation (argument %d) must be a Quaternion", function, index);
        return Quaternion::IDENTITY;
    }

    uint32_t pqOptMask(lua_State* L, int index){
        // Unity's int mask: -1 is every layer
        return lua_isnoneornil(L, index) ? Physics::DEFAULT_RAYCAST_LAYERS : (uint32_t)luaL_checkinteger(L, index);
    }

    QueryTriggerInteraction pqOptTriggers(lua_State* L, int index, const char* function){
        if (lua_isnoneornil(L, index)){
            return QueryTriggerInteraction::UseGlobal;
        }
        const lua_Integer value = luaL_checkinteger(L, index);
        if (value < 0 || value > 2){
            luaL_error(L, "%s: argument %d must be a QueryTriggerInteraction (UseGlobal, Ignore or Collide)", function, index);
        }
        return (QueryTriggerInteraction)value;
    }

    // an entity number or a GameObject (any EntityHandle)
    bool pqEntity(lua_State* L, int index, Entity& entity){
        if (lua_isinteger(L, index)){
            entity = (Entity)lua_tointeger(L, index);
            return true;
        }
        if (luabridge::isInstance<EntityHandle>(L, index)){
            auto handle = luabridge::get<EntityHandle*>(L, index);
            if (handle && handle.value()){
                entity = handle.value()->getEntity();
                return true;
            }
        }
        return false;
    }

    // the optional table of entities the query passes through, into the shared buffer
    std::span<const Entity> pqOptIgnore(lua_State* L, int index, const char* function){
        std::vector<Entity>& ignore = pqIgnoreBuffer();
        ignore.clear();
        if (lua_isnoneornil(L, index)){
            return {};
        }
        luaL_checktype(L, index, LUA_TTABLE);
        const lua_Integer count = (lua_Integer)lua_rawlen(L, index);
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, index, i);
            Entity entity = NULL_ENTITY;
            if (!pqEntity(L, -1, entity)){
                luaL_error(L, "%s: ignore[%d] is not an entity", function, (int)i);
            }
            lua_pop(L, 1);
            ignore.push_back(entity);
        }
        return std::span<const Entity>(ignore.data(), ignore.size());
    }

    int pqOptTable(lua_State* L, int index){
        if (lua_isnoneornil(L, index)){
            return 0;
        }
        luaL_checktype(L, index, LUA_TTABLE);
        return index;
    }

    // t[1..count] = values, t[count + 1..old length] = nil
    template<typename Value>
    void pqFillTable(lua_State* L, int table, size_t count, Value value){
        const lua_Integer old = (lua_Integer)lua_rawlen(L, table);
        for (size_t i = 0; i < count; i++){
            lua_pushinteger(L, value(i));
            lua_rawseti(L, table, (lua_Integer)(i + 1));
        }
        for (lua_Integer i = (lua_Integer)count + 1; i <= old; i++){
            lua_pushnil(L);
            lua_rawseti(L, table, i);
        }
    }

    template<typename T>
    void pqPushArray(lua_State* L, const std::vector<T>& values){
        lua_createtable(L, (int)values.size(), 0);
        for (size_t i = 0; i < values.size(); i++){
            if (!luabridge::push(L, values[i])){
                lua_pushnil(L);
            }
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
    }

    void pqPushRayReturn(lua_State* L, const RayReturn& hit){
        if (!luabridge::push(L, hit)){
            lua_pushnil(L);
        }
    }

    enum class PqForm { First, All, NonAlloc, Check };
    using Shape = physics::QueryShape::Type;

    // The results of an overlap form, pushed; results / colliders: the NonAlloc tables
    int pqReturnOverlap(lua_State* L, PqForm form, const std::vector<OverlapHit>& hits, int results, int colliders){
        if (form == PqForm::NonAlloc){
            pqFillTable(L, results, hits.size(), [&](size_t i){ return (lua_Integer)hits[i].body; });
            if (colliders){
                pqFillTable(L, colliders, hits.size(), [&](size_t i){ return (lua_Integer)hits[i].colliderIndex; });
            }
            lua_pushinteger(L, (lua_Integer)hits.size());
        }else if (form == PqForm::First){
            if (hits.empty()){
                lua_pushnil(L);
            }else if (!luabridge::push(L, hits.front())){
                lua_pushnil(L);
            }
        }else{
            pqPushArray(L, hits);
        }
        return 1;
    }

#ifdef JASPERENGINE_PHYSICS_3D
    const char* pqName3D(Shape shape, PqForm form){
        static const char* names[3][4] = {
            {"Physics.overlapSphere", "Physics.overlapSphere", "Physics.overlapSphereNonAlloc", "Physics.checkSphere"},
            {"Physics.overlapBox", "Physics.overlapBox", "Physics.overlapBoxNonAlloc", "Physics.checkBox"},
            {"Physics.overlapCapsule", "Physics.overlapCapsule", "Physics.overlapCapsuleNonAlloc", "Physics.checkCapsule"}};
        return names[(int)shape][(int)form];
    }

    // Physics.overlapX(scene, shape... [, layerMask [, qti [, ignore]]]) -> {OverlapHit}
    // Physics.overlapXNonAlloc(scene, shape..., results [, layerMask [, qti [, colliders [, ignore]]]]) -> count
    // Physics.checkX(scene, shape... [, layerMask [, qti [, ignore]]]) -> boolean
    // A box's rotation follows its half extents (after results in the NonAlloc form, as Unity).
    template<Shape S, PqForm F>
    int pqOverlap3D(lua_State* L){
        const char* function = pqName3D(S, F);
        Scene* scene = pqScene(L, function);
        physics::QueryShape shape;
        shape.type = S;
        int index = 2;
        if constexpr (S == Shape::Capsule){
            shape.a = pqVector3(L, index++, function, "point0");
            shape.b = pqVector3(L, index++, function, "point1");
            shape.radius = (float)luaL_checknumber(L, index++);
        }else{
            shape.a = pqVector3(L, index++, function, "center");
            if constexpr (S == Shape::Box){
                shape.halfExtents = pqVector3(L, index++, function, "halfExtents");
            }else{
                shape.radius = (float)luaL_checknumber(L, index++);
            }
        }
        int results = 0;
        if constexpr (F == PqForm::NonAlloc){
            luaL_checktype(L, index, LUA_TTABLE);
            results = index++;
        }
        if constexpr (S == Shape::Box){
            shape.rotation = pqOptRotation(L, index++, function);
        }
        PhysicsQueryOptions options;
        options.layerMask = pqOptMask(L, index++);
        options.triggers = pqOptTriggers(L, index++, function);
        int colliders = 0;
        if constexpr (F == PqForm::NonAlloc){
            colliders = pqOptTable(L, index++);
        }
        options.ignore = pqOptIgnore(L, index, function);

        if constexpr (F == PqForm::Check){
            bool found = false;
            if constexpr (S == Shape::Sphere) found = Physics::checkSphere(scene, shape.a, shape.radius, options);
            else if constexpr (S == Shape::Box) found = Physics::checkBox(scene, shape.a, shape.halfExtents, shape.rotation, options);
            else found = Physics::checkCapsule(scene, shape.a, shape.b, shape.radius, options);
            lua_pushboolean(L, found ? 1 : 0);
            return 1;
        }else{
            std::vector<OverlapHit>& hits = pqOverlapBuffer();
            physics::queryOverlap(scene, shape, options, hits);
            return pqReturnOverlap(L, F, hits, results, colliders);
        }
    }

    // Physics.sphereCast(scene, origin, radius, direction, maxDistance [, layerMask [, qti [, ignore]]])
    // Physics.boxCast(scene, center, halfExtents, direction, maxDistance [, rotation [, layerMask [, qti [, ignore]]]])
    // Physics.capsuleCast(scene, point0, point1, radius, direction, maxDistance [, layerMask [, qti [, ignore]]])
    // -> RayReturn (hit false on a miss); the ...All forms -> {RayReturn}, nearest first
    template<Shape S, bool all>
    int pqCast3D(lua_State* L){
        static const char* names[3][2] = {{"Physics.sphereCast", "Physics.sphereCastAll"}, {"Physics.boxCast", "Physics.boxCastAll"},
                                          {"Physics.capsuleCast", "Physics.capsuleCastAll"}};
        const char* function = names[(int)S][all ? 1 : 0];
        Scene* scene = pqScene(L, function);
        Vector3 a, b, halfExtents;
        float radius = 0.0f;
        int index = 2;
        if constexpr (S == Shape::Capsule){
            a = pqVector3(L, index++, function, "point0");
            b = pqVector3(L, index++, function, "point1");
            radius = (float)luaL_checknumber(L, index++);
        }else{
            a = pqVector3(L, index++, function, S == Shape::Box ? "center" : "origin");
            if constexpr (S == Shape::Box){
                halfExtents = pqVector3(L, index++, function, "halfExtents");
            }else{
                radius = (float)luaL_checknumber(L, index++);
            }
        }
        const Vector3 direction = pqVector3(L, index++, function, "direction");
        const float maxDistance = (float)luaL_checknumber(L, index++);
        Quaternion rotation = Quaternion::IDENTITY;
        if constexpr (S == Shape::Box){
            rotation = pqOptRotation(L, index++, function);
        }
        PhysicsQueryOptions options;
        options.layerMask = pqOptMask(L, index++);
        options.triggers = pqOptTriggers(L, index++, function);
        options.ignore = pqOptIgnore(L, index, function);

        std::vector<RayReturn>& hits = pqCastBuffer();
        if constexpr (S == Shape::Sphere){
            if (all) Physics::sphereCastAll(scene, a, radius, direction, maxDistance, hits, options);
            else pqPushRayReturn(L, Physics::sphereCast(scene, a, radius, direction, maxDistance, options));
        }else if constexpr (S == Shape::Box){
            if (all) Physics::boxCastAll(scene, a, halfExtents, direction, maxDistance, rotation, hits, options);
            else pqPushRayReturn(L, Physics::boxCast(scene, a, halfExtents, direction, maxDistance, rotation, options));
        }else{
            if (all) Physics::capsuleCastAll(scene, a, b, radius, direction, maxDistance, hits, options);
            else pqPushRayReturn(L, Physics::capsuleCast(scene, a, b, radius, direction, maxDistance, options));
        }
        if (all){
            pqPushArray(L, hits);
        }
        return 1;
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    Vector2 pqVector2(lua_State* L, int index, const char* function, const char* name){
        if (luabridge::isInstance<Vector2>(L, index)){
            auto result = luabridge::get<Vector2>(L, index);
            if (result){
                return result.value();
            }
        }
        luaL_error(L, "%s: %s (argument %d) must be a Vector2", function, name, index);
        return Vector2(0.0f, 0.0f);
    }

    CapsuleDirection2D pqCapsuleDirection(lua_State* L, int index, const char* function){
        const lua_Integer value = luaL_checkinteger(L, index);
        if (value < 0 || value > 1){
            luaL_error(L, "%s: argument %d must be a CapsuleDirection2D (Vertical or Horizontal)", function, index);
        }
        return (CapsuleDirection2D)value;
    }

    const char* pqName2D(Shape shape, PqForm form){
        static const char* names[5][3] = {
            {"Physics2D.overlapPoint", "Physics2D.overlapPointAll", "Physics2D.overlapPointNonAlloc"},
            {"Physics2D.overlapCircle", "Physics2D.overlapCircleAll", "Physics2D.overlapCircleNonAlloc"},
            {"Physics2D.overlapBox", "Physics2D.overlapBoxAll", "Physics2D.overlapBoxNonAlloc"},
            {"Physics2D.overlapCapsule", "Physics2D.overlapCapsuleAll", "Physics2D.overlapCapsuleNonAlloc"},
            {"Physics2D.overlapArea", "Physics2D.overlapAreaAll", "Physics2D.overlapAreaNonAlloc"}};
        return names[(int)shape - (int)Shape::Point2D][(int)form];
    }

    // the 2D shape's arguments from index 2 (scene units, degrees); returns the next index
    int pqShape2D(lua_State* L, Shape type, const char* function, physics::QueryShape& shape){
        shape.type = type;
        int index = 2;
        auto put = [&](physics::QueryShape& target, const Vector2& value, bool second){
            (second ? target.b : target.a) = Vector3(value.x, value.y, 0.0f);
        };
        switch (type){
            case Shape::Point2D:
                put(shape, pqVector2(L, index++, function, "point"), false);
                break;
            case Shape::Circle2D:
                put(shape, pqVector2(L, index++, function, "center"), false);
                shape.radius = (float)luaL_checknumber(L, index++);
                break;
            case Shape::Box2D: {
                put(shape, pqVector2(L, index++, function, "center"), false);
                const Vector2 size = pqVector2(L, index++, function, "size");
                shape.halfExtents = Vector3(size.x * 0.5f, size.y * 0.5f, 0.0f);
                shape.angle = (float)luaL_optnumber(L, index++, 0.0);
                break;
            }
            case Shape::Capsule2D: {
                put(shape, pqVector2(L, index++, function, "center"), false);
                const Vector2 size = pqVector2(L, index++, function, "size");
                shape.halfExtents = Vector3(size.x * 0.5f, size.y * 0.5f, 0.0f);
                shape.direction2D = pqCapsuleDirection(L, index++, function);
                shape.angle = (float)luaL_optnumber(L, index++, 0.0);
                break;
            }
            default:
                put(shape, pqVector2(L, index++, function, "pointA"), false);
                put(shape, pqVector2(L, index++, function, "pointB"), true);
                break;
        }
        return index;
    }

    // Physics2D.overlapX(scene, shape... [, layerMask [, qti [, ignore]]]) -> OverlapHit or nil
    // Physics2D.overlapXAll(...) -> {OverlapHit}
    // Physics2D.overlapXNonAlloc(scene, shape..., results [, layerMask [, qti [, colliders [, ignore]]]]) -> count
    template<Shape S, PqForm F>
    int pqOverlap2D(lua_State* L){
        const char* function = pqName2D(S, F);
        Scene* scene = pqScene(L, function);
        physics::QueryShape shape;
        int index = pqShape2D(L, S, function, shape);
        int results = 0;
        if constexpr (F == PqForm::NonAlloc){
            luaL_checktype(L, index, LUA_TTABLE);
            results = index++;
        }
        PhysicsQueryOptions options;
        options.layerMask = pqOptMask(L, index++);
        options.triggers = pqOptTriggers(L, index++, function);
        int colliders = 0;
        if constexpr (F == PqForm::NonAlloc){
            colliders = pqOptTable(L, index++);
        }
        options.ignore = pqOptIgnore(L, index, function);

        std::vector<OverlapHit>& hits = pqOverlapBuffer();
        hits.clear();
        if constexpr (F == PqForm::First){
            // the C++ first form: no buffer, the first in (body, collider) order
            const Vector2 a(shape.a.x, shape.a.y);
            const Vector2 size(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
            OverlapHit first;
            if constexpr (S == Shape::Point2D) first = Physics2D::overlapPoint(scene, a, options);
            else if constexpr (S == Shape::Circle2D) first = Physics2D::overlapCircle(scene, a, shape.radius, options);
            else if constexpr (S == Shape::Box2D) first = Physics2D::overlapBox(scene, a, size, shape.angle, options);
            else if constexpr (S == Shape::Capsule2D) first = Physics2D::overlapCapsule(scene, a, size, shape.direction2D, shape.angle, options);
            else first = Physics2D::overlapArea(scene, a, Vector2(shape.b.x, shape.b.y), options);
            if (first.hit){
                hits.push_back(first);
            }
        }else{
            physics::queryOverlap(scene, shape, options, hits);
        }
        return pqReturnOverlap(L, F, hits, results, colliders);
    }

    // Physics2D.circleCast(scene, origin, radius, direction, distance [, layerMask [, qti [, ignore]]])
    // Physics2D.boxCast(scene, origin, size, angle, direction, distance [, ...])
    // Physics2D.capsuleCast(scene, origin, size, capsuleDirection, angle, direction, distance [, ...])
    template<Shape S, bool all>
    int pqCast2D(lua_State* L){
        static const char* names[3][2] = {{"Physics2D.circleCast", "Physics2D.circleCastAll"}, {"Physics2D.boxCast", "Physics2D.boxCastAll"},
                                          {"Physics2D.capsuleCast", "Physics2D.capsuleCastAll"}};
        const char* function = names[(int)S - (int)Shape::Circle2D][all ? 1 : 0];
        Scene* scene = pqScene(L, function);
        physics::QueryShape shape;
        int index = pqShape2D(L, S, function, shape);
        const Vector2 direction = pqVector2(L, index++, function, "direction");
        const float distance = (float)luaL_checknumber(L, index++);
        PhysicsQueryOptions options;
        options.layerMask = pqOptMask(L, index++);
        options.triggers = pqOptTriggers(L, index++, function);
        options.ignore = pqOptIgnore(L, index, function);

        const Vector2 origin(shape.a.x, shape.a.y);
        const Vector2 size(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
        std::vector<RayReturn>& hits = pqCastBuffer();
        if constexpr (S == Shape::Circle2D){
            if (all) Physics2D::circleCastAll(scene, origin, shape.radius, direction, distance, hits, options);
            else pqPushRayReturn(L, Physics2D::circleCast(scene, origin, shape.radius, direction, distance, options));
        }else if constexpr (S == Shape::Box2D){
            if (all) Physics2D::boxCastAll(scene, origin, size, shape.angle, direction, distance, hits, options);
            else pqPushRayReturn(L, Physics2D::boxCast(scene, origin, size, shape.angle, direction, distance, options));
        }else{
            if (all) Physics2D::capsuleCastAll(scene, origin, size, shape.direction2D, shape.angle, direction, distance, hits, options);
            else pqPushRayReturn(L, Physics2D::capsuleCast(scene, origin, size, shape.direction2D, shape.angle, direction, distance, options));
        }
        if (all){
            pqPushArray(L, hits);
        }
        return 1;
    }
#endif

    // { kinds = MeshRaycast.MESH | ..., includeInactive = false, ignore = {e1, e2}, current = false }
    MeshRaycastOptions& pqMeshOptions(lua_State* L, int index, const char* function){
        static MeshRaycastOptions options;
        options = MeshRaycast::overlapOptions();
        options.ignore.clear();
        if (lua_isnoneornil(L, index)){
            return options;
        }
        luaL_checktype(L, index, LUA_TTABLE);
        index = lua_absindex(L, index);
        lua_getfield(L, index, "kinds");
        if (!lua_isnil(L, -1)) options.kinds = (uint32_t)luaL_checkinteger(L, -1) & MeshRaycast::ALL;
        lua_pop(L, 1);
        lua_getfield(L, index, "includeInactive");
        if (!lua_isnil(L, -1)) options.includeInactive = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, index, "current");
        if (!lua_isnil(L, -1)) options.current = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, index, "ignore");
        const int ignore = lua_gettop(L);
        if (!lua_isnil(L, ignore)){
            pqOptIgnore(L, ignore, function);
            options.ignore.assign(pqIgnoreBuffer().begin(), pqIgnoreBuffer().end());
        }
        lua_pop(L, 1);
        return options;
    }

    // MeshRaycast.overlapSphere(scene, center, radius [, layerMask [, options]]) -> {MeshOverlapHit}
    // MeshRaycast.overlapSphereNonAlloc(scene, center, radius, results [, layerMask [, options]]) -> count
    // MeshRaycast.overlapBox(scene, center, halfExtents [, rotation [, layerMask [, options]]]) -> {MeshOverlapHit}
    // MeshRaycast.overlapBoxNonAlloc(scene, center, halfExtents, rotation, results [, layerMask [, options]]) -> count
    template<bool box, bool nonAlloc>
    int pqMeshOverlap(lua_State* L){
        static const char* names[2][2] = {{"MeshRaycast.overlapSphere", "MeshRaycast.overlapSphereNonAlloc"},
                                          {"MeshRaycast.overlapBox", "MeshRaycast.overlapBoxNonAlloc"}};
        const char* function = names[box ? 1 : 0][nonAlloc ? 1 : 0];
        Scene* scene = pqScene(L, function);
        int index = 2;
        const Vector3 center = pqVector3(L, index++, function, "center");
        Vector3 halfExtents;
        float radius = 0.0f;
        Quaternion rotation = Quaternion::IDENTITY;
        if constexpr (box){
            halfExtents = pqVector3(L, index++, function, "halfExtents");
            if (nonAlloc){
                if (!luabridge::isInstance<Quaternion>(L, index)){
                    return luaL_error(L, "%s: rotation (argument %d) must be a Quaternion", function, index);
                }
            }
            rotation = pqOptRotation(L, index++, function);
        }else{
            radius = (float)luaL_checknumber(L, index++);
        }
        int results = 0;
        if constexpr (nonAlloc){
            luaL_checktype(L, index, LUA_TTABLE);
            results = index++;
        }
        const uint32_t layerMask = pqOptMask(L, index++);
        const MeshRaycastOptions& options = pqMeshOptions(L, index, function);

        std::vector<MeshOverlapHit>& hits = pqMeshBuffer();
        if constexpr (box){
            MeshRaycast::overlapBox(scene, center, halfExtents, rotation, hits, layerMask, options);
        }else{
            MeshRaycast::overlapSphere(scene, center, radius, hits, layerMask, options);
        }
        if constexpr (nonAlloc){
            pqFillTable(L, results, hits.size(), [&](size_t i){ return (lua_Integer)hits[i].entity; });
            lua_pushinteger(L, (lua_Integer)hits.size());
        }else{
            pqPushArray(L, hits);
        }
        return 1;
    }

    lua_Integer pqOverlapHitBody(const OverlapHit* hit){ return (lua_Integer)hit->body; }
    lua_Integer pqOverlapHitCollider(const OverlapHit* hit){ return (lua_Integer)hit->colliderIndex; }
    bool pqOverlapHitTrigger(const OverlapHit* hit){ return hit->trigger; }
    lua_Integer pqMeshOverlapEntity(const MeshOverlapHit* hit){ return (lua_Integer)hit->entity; }
    lua_Integer pqMeshOverlapKind(const MeshOverlapHit* hit){ return (lua_Integer)(1u << (uint32_t)hit->kind); }
    lua_Integer pqMeshOverlapInstance(const MeshOverlapHit* hit){ return (lua_Integer)hit->instance; }

}
#endif

void LuaBinding::registerPhysicsQueryClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    // Unity's QueryTriggerInteraction and CapsuleDirection2D (the same values)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("QueryTriggerInteraction")
        .addVariable("UseGlobal", (int)QueryTriggerInteraction::UseGlobal)
        .addVariable("Ignore", (int)QueryTriggerInteraction::Ignore)
        .addVariable("Collide", (int)QueryTriggerInteraction::Collide)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CapsuleDirection2D")
        .addVariable("Vertical", (int)CapsuleDirection2D::Vertical)
        .addVariable("Horizontal", (int)CapsuleDirection2D::Horizontal)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<OverlapHit>("OverlapHit")
        .addProperty("body", &pqOverlapHitBody)
        .addProperty("colliderIndex", &pqOverlapHitCollider)
        .addProperty("isTrigger", &pqOverlapHitTrigger)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<MeshOverlapHit>("MeshOverlapHit")
        .addProperty("entity", &pqMeshOverlapEntity)
        .addProperty("kind", &pqMeshOverlapKind)
        .addProperty("instance", &pqMeshOverlapInstance)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics>("Physics")
        // what QueryTriggerInteraction.UseGlobal means for every 3D query, raycast included
        .addStaticProperty("queriesHitTriggers", &Physics::getQueriesHitTriggers, &Physics::setQueriesHitTriggers)
        // the main thread's recent queries, drawn for 1 s in both views (not saved)
        .addStaticProperty("debugDrawQueries", &Physics::isDebugDrawQueries, &Physics::setDebugDrawQueries)
#ifdef JASPERENGINE_PHYSICS_3D
        .addStaticFunction("overlapSphere", &pqOverlap3D<Shape::Sphere, PqForm::All>)
        .addStaticFunction("overlapBox", &pqOverlap3D<Shape::Box, PqForm::All>)
        .addStaticFunction("overlapCapsule", &pqOverlap3D<Shape::Capsule, PqForm::All>)
        .addStaticFunction("overlapSphereNonAlloc", &pqOverlap3D<Shape::Sphere, PqForm::NonAlloc>)
        .addStaticFunction("overlapBoxNonAlloc", &pqOverlap3D<Shape::Box, PqForm::NonAlloc>)
        .addStaticFunction("overlapCapsuleNonAlloc", &pqOverlap3D<Shape::Capsule, PqForm::NonAlloc>)
        .addStaticFunction("checkSphere", &pqOverlap3D<Shape::Sphere, PqForm::Check>)
        .addStaticFunction("checkBox", &pqOverlap3D<Shape::Box, PqForm::Check>)
        .addStaticFunction("checkCapsule", &pqOverlap3D<Shape::Capsule, PqForm::Check>)
        .addStaticFunction("sphereCast", &pqCast3D<Shape::Sphere, false>)
        .addStaticFunction("boxCast", &pqCast3D<Shape::Box, false>)
        .addStaticFunction("capsuleCast", &pqCast3D<Shape::Capsule, false>)
        .addStaticFunction("sphereCastAll", &pqCast3D<Shape::Sphere, true>)
        .addStaticFunction("boxCastAll", &pqCast3D<Shape::Box, true>)
        .addStaticFunction("capsuleCastAll", &pqCast3D<Shape::Capsule, true>)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics2D>("Physics2D")
        .addStaticProperty("queriesHitTriggers", &Physics2D::getQueriesHitTriggers, &Physics2D::setQueriesHitTriggers)
#ifdef JASPERENGINE_PHYSICS_2D
        .addStaticFunction("overlapPoint", &pqOverlap2D<Shape::Point2D, PqForm::First>)
        .addStaticFunction("overlapPointAll", &pqOverlap2D<Shape::Point2D, PqForm::All>)
        .addStaticFunction("overlapPointNonAlloc", &pqOverlap2D<Shape::Point2D, PqForm::NonAlloc>)
        .addStaticFunction("overlapCircle", &pqOverlap2D<Shape::Circle2D, PqForm::First>)
        .addStaticFunction("overlapCircleAll", &pqOverlap2D<Shape::Circle2D, PqForm::All>)
        .addStaticFunction("overlapCircleNonAlloc", &pqOverlap2D<Shape::Circle2D, PqForm::NonAlloc>)
        .addStaticFunction("overlapBox", &pqOverlap2D<Shape::Box2D, PqForm::First>)
        .addStaticFunction("overlapBoxAll", &pqOverlap2D<Shape::Box2D, PqForm::All>)
        .addStaticFunction("overlapBoxNonAlloc", &pqOverlap2D<Shape::Box2D, PqForm::NonAlloc>)
        .addStaticFunction("overlapCapsule", &pqOverlap2D<Shape::Capsule2D, PqForm::First>)
        .addStaticFunction("overlapCapsuleAll", &pqOverlap2D<Shape::Capsule2D, PqForm::All>)
        .addStaticFunction("overlapCapsuleNonAlloc", &pqOverlap2D<Shape::Capsule2D, PqForm::NonAlloc>)
        .addStaticFunction("overlapArea", &pqOverlap2D<Shape::Area2D, PqForm::First>)
        .addStaticFunction("overlapAreaAll", &pqOverlap2D<Shape::Area2D, PqForm::All>)
        .addStaticFunction("overlapAreaNonAlloc", &pqOverlap2D<Shape::Area2D, PqForm::NonAlloc>)
        .addStaticFunction("circleCast", &pqCast2D<Shape::Circle2D, false>)
        .addStaticFunction("boxCast", &pqCast2D<Shape::Box2D, false>)
        .addStaticFunction("capsuleCast", &pqCast2D<Shape::Capsule2D, false>)
        .addStaticFunction("circleCastAll", &pqCast2D<Shape::Circle2D, true>)
        .addStaticFunction("boxCastAll", &pqCast2D<Shape::Box2D, true>)
        .addStaticFunction("capsuleCastAll", &pqCast2D<Shape::Capsule2D, true>)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<MeshRaycast>("MeshRaycast")
        .addStaticFunction("overlapSphere", &pqMeshOverlap<false, false>)
        .addStaticFunction("overlapSphereNonAlloc", &pqMeshOverlap<false, true>)
        .addStaticFunction("overlapBox", &pqMeshOverlap<true, false>)
        .addStaticFunction("overlapBoxNonAlloc", &pqMeshOverlap<true, true>)
        .endClass();
#else
    (void)L;
#endif
}
