// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Outline component in Lua (docs/specs/S94-rendering-anim-gaps.md F22, AC22): Outline(scene,
// entity) adds it, its properties read and write the component (colour sRGB in Lua, linear
// stored), the OutlineMode and OutlineWidthUnits tables, and a bad width changes nothing.

#include "Scene.h"
#include "component/OutlineComponent.h"
#include "object/Primitive.h"
#include "script/LuaBinding.h"
#include "util/Color.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>

namespace LuaOutlineTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string runOutlineLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaOutline, HandleAddsAndEditsTheComponent){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    luabridge::setGlobal(L, &scene, "outlineScene");
    luabridge::setGlobal(L, (unsigned int)box.getEntity(), "outlineEntity");
    EXPECT_EQ(runOutlineLua(L,
        "local outline = Outline(outlineScene, outlineEntity)\n"
        "assert(outline.enabled == true)\n"
        "assert(outline.mode == OutlineMode.OUTLINE_ALL)\n"
        "assert(outline.widthUnits == OutlineWidthUnits.PIXELS)\n"
        "assert(outline.includeChildren == true)\n"
        "assert(math.abs(outline.width - 2) < 1e-6)\n"
        "outline.mode = OutlineMode.OUTLINE_VISIBLE\n"
        "outline.color = Vector4(1, 0.5, 0, 0.75)\n"
        "outline.width = 0.05\n"
        "outline.widthUnits = OutlineWidthUnits.WORLD\n"
        "outline.includeChildren = false\n"
        "outline.enabled = false\n"
        "local c = outline.color\n"
        "assert(math.abs(c.x - 1) < 1e-4 and math.abs(c.y - 0.5) < 1e-3 and math.abs(c.w - 0.75) < 1e-6)\n"
        "assert(Outline(outlineScene, outlineEntity).mode == OutlineMode.OUTLINE_VISIBLE)\n"), "");
    const OutlineComponent* outline = scene.findComponent<OutlineComponent>(box.getEntity());
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->mode, OutlineMode::OUTLINE_VISIBLE);
    EXPECT_EQ(outline->widthUnits, OutlineWidthUnits::WORLD);
    EXPECT_FLOAT_EQ(outline->width, 0.05f);
    EXPECT_FALSE(outline->includeChildren);
    EXPECT_FALSE(outline->enabled);
    // stored linear, as the engine converts sRGB
    EXPECT_NEAR(outline->color.y, Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).y, 1e-5f);
    EXPECT_LT(outline->color.y, 0.25f);
    EXPECT_FLOAT_EQ(outline->color.w, 0.75f);
}

TEST(LuaOutline, NegativeWidthIsRefused){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    luabridge::setGlobal(L, &scene, "outlineScene");
    luabridge::setGlobal(L, (unsigned int)box.getEntity(), "outlineEntity");
    LogCapture log;
    EXPECT_EQ(runOutlineLua(L, "Outline(outlineScene, outlineEntity).width = -1\n"), "");
    EXPECT_EQ(log.count(D_LOG_ERROR, "Outline width"), 1u);
    EXPECT_FLOAT_EQ(scene.getComponent<OutlineComponent>(box.getEntity()).width, 2.0f);
}

}  // namespace LuaOutlineTest_cpp
