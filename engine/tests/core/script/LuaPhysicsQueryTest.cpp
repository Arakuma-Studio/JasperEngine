// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The spatial queries' Lua beside S96's (docs/specs/spatial-queries.md AC12, R28): the NonAlloc
// forms write GameObject ids and collider indices, clear the rest of the tables and make no
// garbage once they have grown; MeshRaycast's overlaps read trees.

#include "Scene.h"
#include "registry/PhysicsLayers.h"
#include "script/LuaBinding.h"
#include "../physics/QueryCityTestUtil.h"
#include "../terrain/FoliageTestSupport.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

namespace LuaPhysicsQueryTest_cpp {

using namespace jasperengine;

namespace {

    std::string runPhysicsQueryLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

#ifdef JASPERENGINE_PHYSICS_3D
TEST(LuaPhysicsQuery, NonAllocFillsAndClears){
    lua_State* L = LuaBinding::getLuaState();
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    std::vector<Entity> bodies;
    for (int i = 0; i < 6; i++){
        const Entity entity = test::placedEntity(scene, Vector3((float)i * 2.0f, 0, 0));
        RigidbodyComponent body;
        body.type = BodyType::STATIC;
        body.colliders.resize(1);
        body.colliders[0].type = Collider3DType::SPHERE;
        body.colliders[0].radius = 0.5f;
        body.colliders[0].isTrigger = i == 5;
        scene.addComponent<RigidbodyComponent>(entity, body);
        bodies.push_back(entity);
    }
    test::stepScene(scene, 1);
    luabridge::setGlobal(L, &scene, "queryScene");
    lua_pushinteger(L, (lua_Integer)bodies[0]);
    lua_setglobal(L, "FIRST");
    lua_pushinteger(L, (lua_Integer)bodies[5]);
    lua_setglobal(L, "TRIGGER");

    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        local t, colliders = {}, {}
        local n = Physics.overlapSphereNonAlloc(scene, Vector3(5, 0, 0), 20, t, -1, {triggers = QueryTriggerInteraction.COLLIDE}, colliders)
        assert(n == 6, "six bodies, got " .. n)
        assert(#t == 6 and #colliders == 6)
        assert(t[1] == FIRST and math.type(t[1]) == "integer", "GameObject ids in (GameObject, collider) order")
        assert(colliders[1] == 0)
        local array = Physics.overlapSphere(scene, Vector3(5, 0, 0), 20, -1, {triggers = QueryTriggerInteraction.COLLIDE})
        for i = 1, n do
            assert(array[i].gameObject.entity == t[i] and array[i].index == colliders[i], "the array form's colliders, in order: " .. i)
        end
        n = Physics.overlapSphereNonAlloc(scene, Vector3(0, 0, 0), 1, t)
        assert(n == 1 and #t == 1 and t[1] == FIRST and t[2] == nil, "#t == n after fewer hits: " .. #t)
        n = Physics.overlapSphereNonAlloc(scene, Vector3(10, 0, 0), 0.6, t, -1, {triggers = QueryTriggerInteraction.IGNORE})
        assert(n == 0 and #t == 0, "the trigger is skipped with IGNORE")
        assert(Physics.overlapSphereNonAlloc(scene, Vector3(10, 0, 0), 0.6, t) == 1 and t[1] == TRIGGER, "USE_GLOBAL: the trigger")
        assert(Physics.overlapSphereNonAlloc(scene, Vector3(5, 0, 0), 20, t, -1, {ignore = {FIRST}}) == 5, "the ignore list")
        assert(Physics.overlapBoxNonAlloc(scene, Vector3(0, 0, 0), Vector3(1, 1, 1), t) == 1 and t[1] == FIRST, "orientation nil")
        assert(Physics.overlapBoxNonAlloc(scene, Vector3(0, 0, 0), Vector3(1, 1, 1), t, Quaternion.IDENTITY, 0) == 0, "layer mask 0")
        assert(Physics.overlapCapsuleNonAlloc(scene, Vector3(0, -1, 0), Vector3(2, 1, 0), 0.25, t) == 2)
        local ok, err = pcall(Physics.overlapSphereNonAlloc, scene, Vector3(0, 0, 0), 1)
        assert(not ok and tostring(err):find("results"), "a missing results table names it: " .. tostring(err))
        ok, err = pcall(Physics.overlapSphereNonAlloc, scene, Vector3(0, 0, 0), 0, t)
        assert(not ok and tostring(err):find("radius"), "a bad radius names it: " .. tostring(err))
        assert(Physics.debugDrawQueries == false)
    )"), "");

    // no garbage over 1,000 calls once the tables have grown
    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        local t, colliders = {}, {}
        local center = Vector3(5, 0, 0)
        Physics.overlapSphereNonAlloc(scene, center, 20, t, -1, nil, colliders)
        collectgarbage("collect")
        collectgarbage("stop")
        local before = collectgarbage("count")
        for i = 1, 1000 do
            Physics.overlapSphereNonAlloc(scene, center, 20, t, -1, nil, colliders)
        end
        local after = collectgarbage("count")
        collectgarbage("restart")
        assert(after == before, "garbage: " .. (after - before) .. " KiB")
    )"), "");

    lua_pushnil(L);
    lua_setglobal(L, "queryScene");
}
#endif

// R12, R28: a tree a MeshRaycast overlap finds reads its kind, id and prototype; a NonAlloc form
// gives its Terrain; details come when kinds asks
TEST(LuaPhysicsQuery, MeshOverlapReadsTrees){
    lua_State* L = LuaBinding::getLuaState();
    foliagetest::FoliageFixture fixture;
    foliagetest::withTrees(fixture, {{10.0f, 10.0f}});
    fixture.settle();
    const terraincore::TreeId id = terraincore::makeTreeId(0, fixture.foliage->getTreeStore(fixture.entity)->records()[0].id);
    FoliageInstanceInfo pose;
    ASSERT_TRUE(fixture.foliage->getTreePose(fixture.entity, id, pose));
    luabridge::setGlobal(L, fixture.scene.get(), "queryScene");
    luabridge::setGlobal(L, pose.position + Vector3(0.0f, 2.0f, 0.0f), "TREE_MIDDLE");
    lua_pushinteger(L, (lua_Integer)fixture.entity);
    lua_setglobal(L, "TERRAIN");
    lua_pushinteger(L, (lua_Integer)id);
    lua_setglobal(L, "TREE_ID");

    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        local hits = MeshRaycast.overlapSphere(scene, TREE_MIDDLE, 0.5)
        assert(#hits == 1, "one tree, got " .. #hits)
        local hit = hits[1]
        assert(hit.kind == MeshRaycast.TREE and hit.entity == TERRAIN and hit.instance == -1)
        assert(hit.foliageId == TREE_ID and hit.prototype == 0, "id " .. hit.foliageId .. ", prototype " .. hit.prototype)
        local t = {}
        assert(MeshRaycast.overlapSphereNonAlloc(scene, TREE_MIDDLE, 0.5, t) == 1 and t[1] == TERRAIN and #t == 1)
        assert(#MeshRaycast.overlapSphere(scene, TREE_MIDDLE, 0.5, -1, {kinds = MeshRaycast.MESH}) == 0)
        local details = MeshRaycast.overlapSphere(scene, TREE_MIDDLE, 6, -1, {kinds = MeshRaycast.DETAIL})
        assert(#details > 0, "grass when asked")
        assert(details[1].kind == MeshRaycast.DETAIL and details[1].foliageId == 0 and details[1].prototype == 0)
    )"), "");

    lua_pushnil(L);
    lua_setglobal(L, "queryScene");
}

#ifdef JASPERENGINE_PHYSICS_2D
TEST(LuaPhysicsQuery, NonAlloc2DFillsAndClears){
    lua_State* L = LuaBinding::getLuaState();
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    scene.getSystem<PhysicsSystem>()->setPointsToMeterScale2D(64.0f);
    std::vector<Entity> bodies;
    for (int i = 0; i < 3; i++){
        const Entity entity = test::placedEntity(scene, Vector3((float)i * 100.0f, 0, 0));
        Rigidbody2DComponent body;
        body.type = BodyType::STATIC;
        body.colliders.resize(1);
        body.colliders[0].type = Collider2DType::CIRCLE;
        body.colliders[0].radius = 32.0f;
        body.colliders[0].sensor = i == 2;
        scene.addComponent<Rigidbody2DComponent>(entity, body);
        bodies.push_back(entity);
    }
    test::stepScene(scene, 1);
    luabridge::setGlobal(L, &scene, "queryScene");
    lua_pushinteger(L, (lua_Integer)bodies[0]);
    lua_setglobal(L, "BODY");
    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        local t, colliders = {1, 2, 3, 4}, {}
        assert(Physics2D.overlapCircleNonAlloc(scene, Vector2(100, 0), 150, t, -1, nil, colliders) == 3 and #t == 3 and #colliders == 3)
        assert(t[1] == BODY and colliders[1] == 0)
        assert(Physics2D.overlapCircleNonAlloc(scene, Vector2(100, 0), 150, t, -1, {triggers = QueryTriggerInteraction.IGNORE}) == 2 and #t == 2)
        assert(Physics2D.overlapPointNonAlloc(scene, Vector2(0, 0), t) == 1 and t[1] == BODY and #t == 1)
        assert(Physics2D.overlapBoxNonAlloc(scene, Vector2(0, 0), Vector2(64, 32), 15, t) == 1)
        assert(Physics2D.overlapCapsuleNonAlloc(scene, Vector2(0, 0), Vector2(20, 60), CapsuleDirection2D.VERTICAL, 0, t) == 1)
        assert(Physics2D.overlapAreaNonAlloc(scene, Vector2(150, 100), Vector2(-100, -100), t) == 2, "corners in any order")
        assert(Physics2D.overlapCircleNonAlloc(scene, Vector2(500, 500), 10, t) == 0 and #t == 0)
        local ok, err = pcall(Physics2D.overlapCapsuleNonAlloc, scene, Vector2(0, 0), Vector2(20, 60), 7, 0, t)
        assert(not ok and tostring(err):find("capsuleDirection"), tostring(err))
    )"), "");
    lua_pushnil(L);
    lua_setglobal(L, "queryScene");
}
#endif

}  // namespace LuaPhysicsQueryTest_cpp
