// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The subsystem methods a game script reaches (scripting catch-up, Doriax 04501b15b): the newly
// bound ones answer, and an omitted boolean keeps C++'s default true (LuaBridge reads a missing
// bool as false: ActionSystem:actionStart(e) restarted a stopped animation inside its old run).

#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/Transform.h"
#include "component/TweenComponent.h"
#include "subsystem/PhysicsSystem.h"
#include "script/LuaBinding.h"
#include "../ecs/EcsTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    std::string runLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaSubsystems, NewMethodsCallable){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity tween = scene.createEntity();
    scene.addComponent<ActionComponent>(tween);
    scene.addComponent<TweenComponent>(tween);
    scene.getComponent<TweenComponent>(tween).duration = 2.0f;
    const Entity plain = makeTransformEntity(scene);

    luabridge::setGlobal(L, &scene, "subScene");
    lua_pushinteger(L, tween); lua_setglobal(L, "TWEEN");
    lua_pushinteger(L, plain); lua_setglobal(L, "PLAIN");
    EXPECT_EQ(runLua(L, R"(
        local scene = subScene
        assert(scene:getActionSystem():getDuration(TWEEN) == 2, "a 2 s tween")
        local render = scene:getRenderSystem()
        assert(type(render:isAllLoaded()) == "boolean", "isAllLoaded")
        local loaded, total = render:getLoadCount()
        assert(math.type(loaded) == "integer" and math.type(total) == "integer" and loaded <= total, "getLoadCount")
        local meshes = scene:getMeshSystem()
        assert(meshes:hasPendingAsyncModelLoads() == false, "no model loading")
        assert(meshes:isAsyncModelLoadPending(PLAIN, "none.glb") == false, "not pending")
        meshes:cancelAsyncModelLoad(PLAIN, "none.glb")
        meshes:cancelAsyncModelLoads()
        local ui = scene:getUISystem()
        local p = Vector2(3, 4)
        assert(ui:layoutToParentSpace(PLAIN, ui:parentToLayoutSpace(PLAIN, p)) == p, "layout space round trip")
        assert(ui:hasPendingImages() == false, "no images")
        ui:resetButtonStates()
        scene:getAudioSystem():stopSceneSounds()
    )"), "");
}

#ifdef JASPERENGINE_PHYSICS_2D
TEST(LuaSubsystems, PhysicsSubscriptionsByTag){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    luabridge::setGlobal(L, &scene, "subScene");
    EXPECT_EQ(runLua(L, R"(
        local physics = subScene:getPhysicsSystem()
        physics.beginContact2D:add("mine", function() end)
        physics.endContact2D:add("theirs", function() end)
        physics:removeSubscriptionsByTag("mine")
    )"), "");
    PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
    EXPECT_FALSE(physics->beginContact2D.hasSubscribers());
    EXPECT_TRUE(physics->endContact2D.hasSubscribers());

    EXPECT_EQ(runLua(L, "subScene:getPhysicsSystem():clearSubscriptions()"), "");
    EXPECT_FALSE(physics->endContact2D.hasSubscribers());
}
#endif

TEST(LuaSubsystems, ActionStartOmittedNewRunIsTrue){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity animation = scene.createEntity();
    scene.addComponent<ActionComponent>(animation);
    scene.addComponent<AnimationComponent>(animation);
    ActionFrame frame;
    frame.started = true;  // started in the last run
    scene.getComponent<AnimationComponent>(animation).actions.push_back(frame);

    luabridge::setGlobal(L, &scene, "subScene");
    lua_pushinteger(L, animation); lua_setglobal(L, "ANIM");
    EXPECT_EQ(runLua(L, "subScene:getActionSystem():actionStart(ANIM)"), "");
    // a new run, as C++ actionStart(e): its frames start again
    EXPECT_FALSE(scene.getComponent<AnimationComponent>(animation).actions[0].started);
}
