// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The spatial queries in Lua (docs/specs/spatial-queries.md AC12, R28): NonAlloc forms write
// entity ids and clear the rest of the table, make no garbage once it has grown; the enums and
// OverlapHit's fields read; a 2D first form is nil on a miss.

#include "Scene.h"
#include "registry/PhysicsLayers.h"
#include "script/LuaBinding.h"
#include "../physics/QueryCityTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

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
        assert(QueryTriggerInteraction.UseGlobal == 0 and QueryTriggerInteraction.Ignore == 1 and QueryTriggerInteraction.Collide == 2)
        assert(CapsuleDirection2D.Vertical == 0 and CapsuleDirection2D.Horizontal == 1)
        local t, colliders = {}, {}
        local n = Physics.overlapSphereNonAlloc(scene, Vector3(5, 0, 0), 20, t, -1, QueryTriggerInteraction.Collide, colliders)
        assert(n == 6, "six bodies, got " .. n)
        assert(#t == 6 and #colliders == 6)
        assert(t[1] == FIRST and math.type(t[1]) == "integer", "ids in (body, collider) order")
        assert(colliders[1] == 0)
        n = Physics.overlapSphereNonAlloc(scene, Vector3(0, 0, 0), 1, t)
        assert(n == 1 and #t == 1 and t[1] == FIRST and t[2] == nil, "#t == n after fewer hits: " .. #t)
        n = Physics.overlapSphereNonAlloc(scene, Vector3(10, 0, 0), 0.6, t, -1, QueryTriggerInteraction.Ignore)
        assert(n == 0 and #t == 0, "the trigger is skipped with Ignore")
        local hits = Physics.overlapSphere(scene, Vector3(10, 0, 0), 0.6, -1, QueryTriggerInteraction.Collide)
        assert(#hits == 1 and hits[1].body == TRIGGER and hits[1].colliderIndex == 0 and hits[1].isTrigger == true)
        assert(Physics.checkBox(scene, Vector3(0, 0, 0), Vector3(1, 1, 1), Quaternion.IDENTITY) == true)
        assert(Physics.checkSphere(scene, Vector3(0, 5, 0), 1) == false)
        local hit = Physics.sphereCast(scene, Vector3(-5, 0, 0), 0.25, Vector3(1, 0, 0), 20)
        assert(hit.hit and hit.body == FIRST and math.abs(hit.distance - 4.25) < 1e-3, "cast: " .. tostring(hit.distance))
        assert(#Physics.capsuleCastAll(scene, Vector3(-5, -0.2, 0), Vector3(-5, 0.2, 0), 0.25, Vector3(1, 0, 0), 20, -1, QueryTriggerInteraction.Collide) == 6)
        assert(Physics.raycast(scene, Vector3(10, 5, 0), Vector3(0, -1, 0), 10, -1, QueryTriggerInteraction.Ignore).hit == false)
        assert(Physics.raycast(scene, Vector3(10, 5, 0), Vector3(0, -1, 0), 10).hit == true)
        assert(#Physics.overlapSphere(scene, Vector3(5, 0, 0), 20, -1, QueryTriggerInteraction.Collide, {FIRST}) == 5, "the ignore list")
        Physics.queriesHitTriggers = false
        assert(#Physics.overlapSphere(scene, Vector3(10, 0, 0), 0.6) == 0)
        Physics.queriesHitTriggers = true
        local ok = pcall(Physics.overlapSphere, scene, Vector3(0, 0, 0), 1, -1, 7)
        assert(not ok, "a bad interaction is an error")
    )"), "");

    // no garbage over 1,000 calls once the table has grown
    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        local t = {}
        Physics.overlapSphereNonAlloc(scene, Vector3(5, 0, 0), 20, t)
        collectgarbage("collect")
        collectgarbage("stop")
        local before = collectgarbage("count")
        local center = Vector3(5, 0, 0)
        for i = 1, 1000 do
            Physics.overlapSphereNonAlloc(scene, center, 20, t)
        end
        local after = collectgarbage("count")
        collectgarbage("restart")
        assert(after == before, "garbage: " .. (after - before) .. " KiB")
    )"), "");

    lua_pushnil(L);
    lua_setglobal(L, "queryScene");
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
TEST(LuaPhysicsQuery, FirstForm2DIsNilOnAMiss){
    lua_State* L = LuaBinding::getLuaState();
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    scene.getSystem<PhysicsSystem>()->setPointsToMeterScale2D(64.0f);
    const Entity entity = test::placedEntity(scene, Vector3(0, 0, 0));
    Rigidbody2DComponent body;
    body.type = BodyType::STATIC;
    body.colliders.resize(1);
    body.colliders[0].type = Collider2DType::CIRCLE;
    body.colliders[0].radius = 32.0f;
    scene.addComponent<Rigidbody2DComponent>(entity, body);
    test::stepScene(scene, 1);
    luabridge::setGlobal(L, &scene, "queryScene");
    lua_pushinteger(L, (lua_Integer)entity);
    lua_setglobal(L, "BODY");
    EXPECT_EQ(runPhysicsQueryLua(L, R"(
        local scene = queryScene
        assert(Physics2D.overlapCircle(scene, Vector2(500, 0), 10) == nil)
        local first = Physics2D.overlapCircle(scene, Vector2(40, 0), 10)
        assert(first ~= nil and first.body == BODY and first.isTrigger == false)
        assert(#Physics2D.overlapBoxAll(scene, Vector2(0, 0), Vector2(64, 32), 15) == 1)
        local t = {1, 2, 3}
        assert(Physics2D.overlapCapsuleNonAlloc(scene, Vector2(0, 0), Vector2(20, 60), CapsuleDirection2D.Vertical, 0, t) == 1 and #t == 1)
        assert(Physics2D.overlapPoint(scene, Vector2(0, 0)).body == BODY)
        assert(#Physics2D.overlapAreaAll(scene, Vector2(-100, -100), Vector2(100, 100)) == 1)
        local hit = Physics2D.circleCast(scene, Vector2(-200, 0), 8, Vector2(1, 0), 400)
        assert(hit.hit and hit.body == BODY and math.abs(hit.distance - 160) < 0.5, tostring(hit.distance))
    )"), "");
    lua_pushnil(L);
    lua_setglobal(L, "queryScene");
}
#endif
