// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The spatial queries' Lua beside S96's (docs/specs/spatial-queries.md R28; S96's casts, overlaps,
// enums and options are PhysicsQueriesLua.cpp): the Physics and Physics2D ...NonAlloc overlaps,
// Physics.debugDrawQueries, MeshRaycast's bounds overlaps, MeshRaycast.TREE / DETAIL and
// MeshOverlapHit. Reopens the Physics, Physics2D and MeshRaycast classes registered before it.
//
// The ...NonAlloc forms write GameObject ids (integers: no userdata, no garbage) into the caller's
// table and clear its entries past the count, so #results is the count. The results go through
// one main-thread buffer: a query with a table that has room allocates nothing.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so every argument is read before a
// local with a destructor exists.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "object/EntityHandle.h"
#include "picking/MeshRaycast.h"
#include "registry/PhysicsLayers.h"
#include "script/binding/PhysicsQueriesLua.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Collision.h"
#include "physics/PhysicsQueries3D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Collision2D.h"
#include "physics/PhysicsQueries2D.h"
#endif

#include <cmath>
#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    std::vector<MeshOverlapHit>& sqMeshBuffer(){
        static std::vector<MeshOverlapHit> hits;
        return hits;
    }

    std::vector<Entity>& sqIgnoreBuffer(){
        static std::vector<Entity> entities;
        return entities;
    }

    Scene* sqScene(lua_State* L, const char* function){
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
    T sqValue(lua_State* L, int index, const char* function, const char* name, const char* type){
        if (luabridge::isInstance<T>(L, index)){
            auto result = luabridge::get<T>(L, index);
            if (result){
                return result.value();
            }
        }
        luaL_error(L, "%s: argument %d (%s) must be a %s", function, index, name, type);
        return T();
    }

    Vector3 sqVector3(lua_State* L, int index, const char* function, const char* name){
        return sqValue<Vector3>(L, index, function, name, "Vector3");
    }

    // nil is Quaternion.IDENTITY (Unity's default orientation)
    Quaternion sqOptRotation(lua_State* L, int index, const char* function){
        if (lua_isnoneornil(L, index)){
            return Quaternion::IDENTITY;
        }
        return sqValue<Quaternion>(L, index, function, "orientation", "Quaternion");
    }

    [[maybe_unused]] float sqNumber(lua_State* L, int index, const char* function, const char* name){
        if (!lua_isnumber(L, index)){
            luaL_error(L, "%s: argument %d (%s) must be a number", function, index, name);
        }
        return (float)lua_tonumber(L, index);
    }

    [[maybe_unused]] void sqCheck(lua_State* L, const char* function, const char* argument, const char* why){
        if (why){
            luaL_error(L, "%s: %s (%s)", function, why, argument);
        }
    }

    // the results table at `index` (required) and the colliders table (optional)
    int sqTable(lua_State* L, int index, const char* function, const char* name, bool optional){
        if (optional && lua_isnoneornil(L, index)){
            return 0;
        }
        if (!lua_istable(L, index)){
            luaL_error(L, "%s: argument %d (%s) must be a table", function, index, name);
        }
        return lua_absindex(L, index);
    }

    uint32_t sqOptMask(lua_State* L, int index, const char* function){
        // Unity's int mask: -1 is every layer
        if (lua_isnoneornil(L, index)){
            return Physics::DEFAULT_RAYCAST_LAYERS;
        }
        if (!lua_isinteger(L, index)){
            luaL_error(L, "%s: argument %d (layerMask) must be an integer", function, index);
        }
        return (uint32_t)lua_tointeger(L, index);
    }

    // an entity number or a GameObject (any EntityHandle)
    bool sqEntity(lua_State* L, int index, Entity& entity){
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

    // the table of entities at `index` into the shared buffer
    void sqIgnore(lua_State* L, int index, const char* function){
        std::vector<Entity>& ignore = sqIgnoreBuffer();
        ignore.clear();
        luaL_checktype(L, index, LUA_TTABLE);
        const lua_Integer count = (lua_Integer)lua_rawlen(L, index);
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, index, i);
            Entity entity = NULL_ENTITY;
            if (!sqEntity(L, -1, entity)){
                luaL_error(L, "%s: ignore[%d] is not an entity", function, (int)i);
            }
            lua_pop(L, 1);
            ignore.push_back(entity);
        }
    }

    // t[1..count] = values, t[count + 1..old length] = nil
    template<typename Value>
    void sqFillTable(lua_State* L, int table, size_t count, Value value){
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

    // A NonAlloc overlap's answer: the colliders' GameObjects into results, their indices into
    // colliders (when given), the count pushed
    template<typename Ref>
    [[maybe_unused]] int sqReturnColliders(lua_State* L, const std::vector<Ref>& found, int results, int colliders){
        sqFillTable(L, results, found.size(), [&](size_t i){ return (lua_Integer)found[i].entity; });
        if (colliders){
            sqFillTable(L, colliders, found.size(), [&](size_t i){ return (lua_Integer)found[i].index; });
        }
        lua_pushinteger(L, (lua_Integer)found.size());
        return 1;
    }

#ifdef JASPERENGINE_PHYSICS_3D
    std::vector<ColliderRef>& sqColliderBuffer(){
        static std::vector<ColliderRef> colliders;
        return colliders;
    }

    enum class SqShape3D { SPHERE, BOX, CAPSULE };

    // Physics.overlapSphereNonAlloc(scene, center, radius, results [, layerMask [, options [, colliders]]])
    // Physics.overlapBoxNonAlloc(scene, center, halfExtents, results [, orientation [, layerMask [, options [, colliders]]]])
    // Physics.overlapCapsuleNonAlloc(scene, point1, point2, radius, results [, layerMask [, options [, colliders]]])
    // -> count. Unity's argument order (results after the shape, a box's orientation after results).
    template<SqShape3D S>
    int sqOverlapNonAlloc3D(lua_State* L){
        static const char* names[3] = {"Physics.overlapSphereNonAlloc", "Physics.overlapBoxNonAlloc", "Physics.overlapCapsuleNonAlloc"};
        const char* function = names[(int)S];
        Scene* scene = sqScene(L, function);
        Vector3 a, b;
        float radius = 0.0f;
        Quaternion orientation = Quaternion::IDENTITY;
        int index = 2;
        if constexpr (S == SqShape3D::CAPSULE){
            a = sqVector3(L, index++, function, "point1");
            b = sqVector3(L, index++, function, "point2");
            radius = sqNumber(L, index++, function, "radius");
            sqCheck(L, function, "point1", PhysicsQueries3D::invalidPoint(a));
            sqCheck(L, function, "point2", PhysicsQueries3D::invalidPoint(b));
            sqCheck(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
        }else{
            a = sqVector3(L, index++, function, "center");
            sqCheck(L, function, "center", PhysicsQueries3D::invalidPoint(a));
            if constexpr (S == SqShape3D::BOX){
                b = sqVector3(L, index++, function, "halfExtents");
                sqCheck(L, function, "halfExtents", PhysicsQueries3D::invalidHalfExtents(b));
            }else{
                radius = sqNumber(L, index++, function, "radius");
                sqCheck(L, function, "radius", PhysicsQueries3D::invalidRadius(radius));
            }
        }
        const int results = sqTable(L, index++, function, "results", false);
        if constexpr (S == SqShape3D::BOX){
            orientation = sqOptRotation(L, index++, function);
        }
        const uint32_t layerMask = sqOptMask(L, index++, function);
        const int optionsIndex = index++;
        const int colliders = sqTable(L, index, function, "colliders", true);

        QueryOptions options;
        std::vector<Entity> ignore;
        luaphysics::readQueryOptions(L, optionsIndex, function, false, options, ignore);
        options.layerMask = layerMask;
        std::vector<ColliderRef>& found = sqColliderBuffer();
        if constexpr (S == SqShape3D::SPHERE) PhysicsQueries3D::overlapSphere(scene, a, radius, found, options);
        else if constexpr (S == SqShape3D::BOX) PhysicsQueries3D::overlapBox(scene, a, b, orientation, found, options);
        else PhysicsQueries3D::overlapCapsule(scene, a, b, radius, found, options);
        return sqReturnColliders(L, found, results, colliders);
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    std::vector<ColliderRef2D>& sqColliderBuffer2D(){
        static std::vector<ColliderRef2D> colliders;
        return colliders;
    }

    enum class SqShape2D { POINT, CIRCLE, BOX, CAPSULE, AREA };

    Vector2 sqVector2(lua_State* L, int index, const char* function, const char* name){
        return sqValue<Vector2>(L, index, function, name, "Vector2");
    }

    // Physics2D.overlapPointNonAlloc(scene, point, results ...), overlapCircleNonAlloc(scene,
    // point, radius, results ...), overlapBoxNonAlloc(scene, point, size, angle, results ...),
    // overlapCapsuleNonAlloc(scene, point, size, capsuleDirection, angle, results ...),
    // overlapAreaNonAlloc(scene, pointA, pointB, results ...), each then
    // [, layerMask [, options [, colliders]]] -> count: overlap*All's colliders
    template<SqShape2D S>
    int sqOverlapNonAlloc2D(lua_State* L){
        static const char* names[5] = {"Physics2D.overlapPointNonAlloc", "Physics2D.overlapCircleNonAlloc", "Physics2D.overlapBoxNonAlloc",
                                       "Physics2D.overlapCapsuleNonAlloc", "Physics2D.overlapAreaNonAlloc"};
        const char* function = names[(int)S];
        Scene* scene = sqScene(L, function);
        int index = 2;
        const Vector2 point = sqVector2(L, index++, function, S == SqShape2D::AREA ? "pointA" : "point");
        sqCheck(L, function, S == SqShape2D::AREA ? "pointA" : "point", PhysicsQueries2D::invalidPoint(point));
        Vector2 size, pointB;
        float radius = 0.0f;
        float angle = 0.0f;
        CapsuleDirection2D capsuleDirection = CapsuleDirection2D::VERTICAL;
        if constexpr (S == SqShape2D::CIRCLE){
            radius = sqNumber(L, index++, function, "radius");
            sqCheck(L, function, "radius", PhysicsQueries2D::invalidRadius(radius));
        }else if constexpr (S == SqShape2D::BOX || S == SqShape2D::CAPSULE){
            size = sqVector2(L, index++, function, "size");
            sqCheck(L, function, "size", PhysicsQueries2D::invalidSize(size));
            if constexpr (S == SqShape2D::CAPSULE){
                const lua_Integer value = lua_isinteger(L, index) ? lua_tointeger(L, index) : -1;
                if (value != (lua_Integer)CapsuleDirection2D::VERTICAL && value != (lua_Integer)CapsuleDirection2D::HORIZONTAL){
                    return luaL_error(L, "%s: argument %d (capsuleDirection) must be CapsuleDirection2D.VERTICAL or HORIZONTAL", function, index);
                }
                capsuleDirection = (CapsuleDirection2D)value;
                index++;
            }
            angle = sqNumber(L, index++, function, "angle");
        }else if constexpr (S == SqShape2D::AREA){
            pointB = sqVector2(L, index++, function, "pointB");
            sqCheck(L, function, "pointB", PhysicsQueries2D::invalidPoint(pointB));
            sqCheck(L, function, "pointA, pointB", PhysicsQueries2D::invalidSize(Vector2(std::fabs(pointB.x - point.x), std::fabs(pointB.y - point.y))));
        }
        const int results = sqTable(L, index++, function, "results", false);
        const uint32_t layerMask = sqOptMask(L, index++, function);
        const int optionsIndex = index++;
        const int colliders = sqTable(L, index, function, "colliders", true);

        QueryOptions options;
        std::vector<Entity> ignore;
        luaphysics::readQueryOptions(L, optionsIndex, function, true, options, ignore);
        options.layerMask = layerMask;
        std::vector<ColliderRef2D>& found = sqColliderBuffer2D();
        if constexpr (S == SqShape2D::POINT) PhysicsQueries2D::overlapPointAll(scene, point, found, options);
        else if constexpr (S == SqShape2D::CIRCLE) PhysicsQueries2D::overlapCircleAll(scene, point, radius, found, options);
        else if constexpr (S == SqShape2D::BOX) PhysicsQueries2D::overlapBoxAll(scene, point, size, angle, found, options);
        else if constexpr (S == SqShape2D::CAPSULE) PhysicsQueries2D::overlapCapsuleAll(scene, point, size, capsuleDirection, angle, found, options);
        else PhysicsQueries2D::overlapAreaAll(scene, point, pointB, found, options);
        return sqReturnColliders(L, found, results, colliders);
    }
#endif

    // { kinds = MeshRaycast.MESH | ..., includeInactive = false, ignore = {e1, e2}, current = false }
    MeshRaycastOptions& sqMeshOptions(lua_State* L, int index, const char* function){
        static MeshRaycastOptions options;
        options = MeshRaycast::overlapOptions();
        options.ignore.clear();
        if (lua_isnoneornil(L, index)){
            return options;
        }
        luaL_checktype(L, index, LUA_TTABLE);
        index = lua_absindex(L, index);
        lua_getfield(L, index, "kinds");
        if (!lua_isnil(L, -1)) options.kinds = (uint32_t)luaL_checkinteger(L, -1) & (MeshRaycast::ALL | MeshRaycast::DETAIL);
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
            sqIgnore(L, ignore, function);
            options.ignore.assign(sqIgnoreBuffer().begin(), sqIgnoreBuffer().end());
        }
        lua_pop(L, 1);
        return options;
    }

    // MeshRaycast.overlapSphere(scene, center, radius [, layerMask [, options]]) -> {MeshOverlapHit}
    // MeshRaycast.overlapSphereNonAlloc(scene, center, radius, results [, layerMask [, options]]) -> count
    // MeshRaycast.overlapBox(scene, center, halfExtents [, rotation [, layerMask [, options]]]) -> {MeshOverlapHit}
    // MeshRaycast.overlapBoxNonAlloc(scene, center, halfExtents, rotation, results [, layerMask [, options]]) -> count
    template<bool box, bool nonAlloc>
    int sqMeshOverlap(lua_State* L){
        static const char* names[2][2] = {{"MeshRaycast.overlapSphere", "MeshRaycast.overlapSphereNonAlloc"},
                                          {"MeshRaycast.overlapBox", "MeshRaycast.overlapBoxNonAlloc"}};
        const char* function = names[box ? 1 : 0][nonAlloc ? 1 : 0];
        Scene* scene = sqScene(L, function);
        int index = 2;
        const Vector3 center = sqVector3(L, index++, function, "center");
        Vector3 halfExtents;
        float radius = 0.0f;
        Quaternion rotation = Quaternion::IDENTITY;
        if constexpr (box){
            halfExtents = sqVector3(L, index++, function, "halfExtents");
            if (nonAlloc){
                if (!luabridge::isInstance<Quaternion>(L, index)){
                    return luaL_error(L, "%s: rotation (argument %d) must be a Quaternion", function, index);
                }
            }
            rotation = sqOptRotation(L, index++, function);
        }else{
            radius = (float)luaL_checknumber(L, index++);
        }
        int results = 0;
        if constexpr (nonAlloc){
            results = sqTable(L, index++, function, "results", false);
        }
        const uint32_t layerMask = sqOptMask(L, index++, function);
        const MeshRaycastOptions& options = sqMeshOptions(L, index, function);

        std::vector<MeshOverlapHit>& hits = sqMeshBuffer();
        if constexpr (box){
            MeshRaycast::overlapBox(scene, center, halfExtents, rotation, hits, layerMask, options);
        }else{
            MeshRaycast::overlapSphere(scene, center, radius, hits, layerMask, options);
        }
        if constexpr (nonAlloc){
            sqFillTable(L, results, hits.size(), [&](size_t i){ return (lua_Integer)hits[i].entity; });
            lua_pushinteger(L, (lua_Integer)hits.size());
        }else{
            lua_createtable(L, (int)hits.size(), 0);
            for (size_t i = 0; i < hits.size(); i++){
                if (!luabridge::push(L, hits[i])){
                    lua_pushnil(L);
                }
                lua_rawseti(L, -2, (lua_Integer)(i + 1));
            }
        }
        return 1;
    }

    lua_Integer sqMeshOverlapEntity(const MeshOverlapHit* hit){ return (lua_Integer)hit->entity; }
    lua_Integer sqMeshOverlapKind(const MeshOverlapHit* hit){ return (lua_Integer)(1u << (uint32_t)hit->kind); }
    lua_Integer sqMeshOverlapInstance(const MeshOverlapHit* hit){ return (lua_Integer)hit->instance; }
    lua_Integer sqMeshRaycastTree(){ return MeshRaycast::TREE; }
    lua_Integer sqMeshRaycastDetail(){ return MeshRaycast::DETAIL; }
    lua_Integer sqMeshOverlapFoliageId(const MeshOverlapHit* hit){ return (lua_Integer)hit->foliageId; }
    lua_Integer sqMeshOverlapPrototype(const MeshOverlapHit* hit){ return (lua_Integer)hit->prototype; }

}
#endif

void LuaBinding::registerSpatialQueryClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    // what MeshRaycast's overlaps return (its fields: docs/scripting/spatial-queries.md)
    luabridge::getGlobalNamespace(L)
        .beginClass<MeshOverlapHit>("MeshOverlapHit")
        .addProperty("entity", &sqMeshOverlapEntity)
        .addProperty("kind", &sqMeshOverlapKind)
        .addProperty("instance", &sqMeshOverlapInstance)
        // TREE: the tree's id on its Terrain (Terrain:removeTreeInstance takes it); 0 otherwise
        .addProperty("foliageId", &sqMeshOverlapFoliageId)
        // TREE: the tree prototype's index; DETAIL: the detail layer's; -1 otherwise
        .addProperty("prototype", &sqMeshOverlapPrototype)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics>("Physics")
        // draws the main thread's queries for 1 s through the physics debug lines (not saved)
        .addStaticProperty("debugDrawQueries", &Physics::isDebugDrawQueries, &Physics::setDebugDrawQueries)
#ifdef JASPERENGINE_PHYSICS_3D
        .addStaticFunction("overlapSphereNonAlloc", &sqOverlapNonAlloc3D<SqShape3D::SPHERE>)
        .addStaticFunction("overlapBoxNonAlloc", &sqOverlapNonAlloc3D<SqShape3D::BOX>)
        .addStaticFunction("overlapCapsuleNonAlloc", &sqOverlapNonAlloc3D<SqShape3D::CAPSULE>)
#endif
        .endClass();

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginClass<Physics2D>("Physics2D")
        .addStaticFunction("overlapPointNonAlloc", &sqOverlapNonAlloc2D<SqShape2D::POINT>)
        .addStaticFunction("overlapCircleNonAlloc", &sqOverlapNonAlloc2D<SqShape2D::CIRCLE>)
        .addStaticFunction("overlapBoxNonAlloc", &sqOverlapNonAlloc2D<SqShape2D::BOX>)
        .addStaticFunction("overlapCapsuleNonAlloc", &sqOverlapNonAlloc2D<SqShape2D::CAPSULE>)
        .addStaticFunction("overlapAreaNonAlloc", &sqOverlapNonAlloc2D<SqShape2D::AREA>)
        .endClass();
#endif

    luabridge::getGlobalNamespace(L)
        .beginClass<MeshRaycast>("MeshRaycast")
        // a Terrain's trees: a default kind of rays and overlaps
        .addStaticProperty("TREE", &sqMeshRaycastTree)
        // a Terrain's details (grass): only when kinds asks
        .addStaticProperty("DETAIL", &sqMeshRaycastDetail)
        .addStaticFunction("overlapSphere", &sqMeshOverlap<false, false>)
        .addStaticFunction("overlapSphereNonAlloc", &sqMeshOverlap<false, true>)
        .addStaticFunction("overlapBox", &sqMeshOverlap<true, false>)
        .addStaticFunction("overlapBoxNonAlloc", &sqMeshOverlap<true, true>)
        .endClass();
#else
    (void)L;
#endif
}
