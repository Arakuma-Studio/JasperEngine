// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua side of S07: Scene:findEntitiesWith, findChangedEntities, markChanged and
// getChangeTick, and the 256-character signature string (64-character strings accepted).

#include "Scene.h"
#include "script/LuaBinding.h"
#include "EcsTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    // runs a chunk; "" on success, else the error message
    std::string runLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaEcsTest, QueriesChangeTicksAndSignatures){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity a = makeTransformEntity(scene);
    const Entity b = makeTransformEntity(scene, a);
    const Entity c = makeTransformEntity(scene);
    // b's mesh first: the Mesh array still follows the Transforms (a before b)
    scene.addComponent<MeshComponent>(b);
    scene.addComponent<MeshComponent>(a);

    luabridge::setGlobal(L, &scene, "ecsScene");
    lua_pushinteger(L, a); lua_setglobal(L, "A");
    lua_pushinteger(L, b); lua_setglobal(L, "B");
    lua_pushinteger(L, c); lua_setglobal(L, "C");

    const std::string script = R"(
        local scene = ecsScene
        local both = scene:findEntitiesWith({"Transform", "MeshComponent"})
        assert(#both == 2, "two with a mesh, got " .. #both)
        assert(both[1] == A and both[2] == B, "Transform order")
        local meshes = scene:findEntitiesWith({"MeshComponent"})
        assert(meshes[1] == A and meshes[2] == B, "Mesh array in Transform order")
        local plain = scene:findEntitiesWith({"Transform"}, {"MeshComponent"})
        assert(#plain == 1 and plain[1] == C, "exclude")
        assert(#scene:findEntitiesWith({"NoSuchComponent"}) == 0, "unknown name")

        local tick = scene:getChangeTick()
        assert(type(tick) == "number" and tick > 0)
        assert(#scene:findChangedEntities("MeshComponent", tick) == 0, "nothing changed yet")
        assert(scene:markChanged(A, "MeshComponent"), "A has a mesh")
        assert(not scene:markChanged(C, "MeshComponent"), "C has none")
        local changed = scene:findChangedEntities("MeshComponent", tick)
        assert(#changed == 1 and changed[1] == A, "A changed")
        assert(scene:getChangeTick() > tick)

        local signature = scene:getSignature(A)
        assert(#signature == 256, "256 characters, got " .. #signature)
        local manager = EntityManager()
        local entity = manager:createUserEntity()
        manager:setSignature(entity, signature)
        assert(manager:getSignature(entity) == signature, "round trip")
        manager:setSignature(entity, string.rep("0", 62) .. "11")
        assert(manager:getSignature(entity) == string.rep("0", 254) .. "11", "64-character string")
    )";
    EXPECT_EQ(runLua(L, script), "");

    lua_pushnil(L);
    lua_setglobal(L, "ecsScene");
    runLua(L, "collectgarbage()");
}
