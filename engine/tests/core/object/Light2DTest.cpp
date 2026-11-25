// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A Light 2D made new, in C++ or Lua (Light2D(scene)), starts at height 64, where normal maps
// respond, as one the editor makes; wrapping an existing light (Light2D(scene, entity)) keeps
// its stored height, and the component's own default stays 0 (docs/specs/S78-sprite-normals-improvements.md R16).

#include "Scene.h"
#include "component/Light2DComponent.h"
#include "object/Light2D.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    std::string runLight2DLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(Light2D, NewLightStartsAtHeight64){
    Scene scene;
    Light2D light(&scene);
    EXPECT_EQ(light.getHeight(), 64.0f);
    EXPECT_EQ(scene.getComponent<Light2DComponent>(light.getEntity()).height, 64.0f);
    EXPECT_EQ(Light2DComponent().height, 0.0f) << "the component's default (the loader's) stays 0";
}

TEST(Light2D, WrappingAnExistingLightKeepsItsHeight){
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Light2DComponent>(entity, {});
    Light2D wrapped(&scene, entity);
    EXPECT_EQ(wrapped.getHeight(), 0.0f);
    scene.getComponent<Light2DComponent>(entity).height = 12.0f;
    Light2D again(&scene, entity);
    EXPECT_EQ(again.getHeight(), 12.0f);
}

TEST(Light2D, LuaConstructorStartsAt64AndWrapKeepsZero){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    const Entity stored = scene.createEntity();
    scene.addComponent<Light2DComponent>(stored, {});
    luabridge::setGlobal(L, &scene, "lightScene");
    lua_pushinteger(L, stored);
    lua_setglobal(L, "STORED_LIGHT");
    EXPECT_EQ(runLight2DLua(L,
        "local made = Light2D(lightScene)\n"
        "assert(made.height == 64, 'Light2D(scene).height is ' .. tostring(made.height))\n"
        "local wrapped = Light2D(lightScene, STORED_LIGHT)\n"
        "assert(wrapped.height == 0, 'Light2D(scene, entity).height is ' .. tostring(wrapped.height))\n"), "");
    EXPECT_EQ(scene.getComponent<Light2DComponent>(stored).height, 0.0f);
    runLight2DLua(L, "lightScene = nil\nSTORED_LIGHT = nil\ncollectgarbage()\n");
}
