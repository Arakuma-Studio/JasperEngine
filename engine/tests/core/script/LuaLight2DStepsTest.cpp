// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S78 R1: the scene's Light Steps in Lua, Scene.light2DSteps (a property, 0 = off, clamped to 16).

#include "Scene.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    std::string runLightStepsLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaLight2DSteps, SceneProperty){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    luabridge::setGlobal(L, &scene, "stepsScene");
    EXPECT_EQ(runLightStepsLua(L,
        "assert(stepsScene.light2DSteps == 0)\n"
        "stepsScene.light2DSteps = 6\n"
        "assert(stepsScene.light2DSteps == 6)\n"
        "stepsScene.light2DSteps = 100\n"
        "assert(stepsScene.light2DSteps == 16)\n"), "");
    EXPECT_EQ(scene.getLight2DSteps(), 16);
    scene.setLight2DSteps(3);
    EXPECT_EQ(runLightStepsLua(L, "assert(stepsScene.light2DSteps == 3)\n"), "");
}
