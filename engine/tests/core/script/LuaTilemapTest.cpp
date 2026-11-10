// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Grid tilemaps in Lua (docs/specs/S50-grid-tilemap.md R79, R80, §6.4): Grid(scene, entity) adds
// a Grid as the other component wrappers do, a script field typed Grid gets a Grid, fill past the
// region limit is a typed error (at the int32 extremes too), and calls on a GameObject without the
// component are Lua errors instead of C++ exceptions through Lua's C frames.

#include "Scene.h"
#include "component/GridComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "object/tilemap/Grid.h"
#include "object/tilemap/TilemapLayer.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <exception>
#include <string>

using namespace jasperengine;

namespace {

    // "" on success, else the Lua error; a C++ exception that left Lua is reported as such
    std::string runTilemapLua(lua_State* L, const std::string& code){
        try{
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
        }catch (const std::exception& e){
            return std::string("C++ exception through Lua: ") + e.what();
        }
        return std::string();
    }

    struct TilemapLuaGlobals {
        lua_State* L;
        TilemapLuaGlobals(lua_State* state, Scene* scene): L(state){ luabridge::setGlobal(L, scene, "tmScene"); }
        void entity(const char* name, Entity entity){
            lua_pushinteger(L, entity);
            lua_setglobal(L, name);
        }
        ~TilemapLuaGlobals(){
            lua_pushnil(L); lua_setglobal(L, "tmScene");
            lua_pushnil(L); lua_setglobal(L, "TM_ENTITY");
            lua_pushnil(L); lua_setglobal(L, "TM_LAYER");
        }
    };

    Entity tilemapPlainEntity(Scene& scene){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, Transform());
        return entity;
    }

}

// a script field or reference typed Grid resolves to the Grid wrapper, not a bare EntityHandle
TEST(LuaTilemap, GridDispatchesByTypeName){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    const Entity entity = tilemapPlainEntity(scene);
    scene.addComponent<GridComponent>(entity, GridComponent());
    const int top = lua_gettop(L);
    ASSERT_TRUE(LuaBinding::pushEntityHandleByType(L, &scene, entity, "Grid"));
    EXPECT_TRUE(luabridge::detail::Userdata::isInstance<Grid>(L, -1));
    lua_settop(L, top);
}

// the codebase's component wrappers (SortingGroup, CanvasGroup, Animator, PixelPerfectCamera...)
// add their component when the GameObject has none, and so does Grid (§6.4)
TEST(LuaTilemap, GridOnAGameObjectWithoutOneAddsIt){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    const Entity entity = tilemapPlainEntity(scene);
    TilemapLuaGlobals globals(L, &scene);
    globals.entity("TM_ENTITY", entity);
    EXPECT_EQ(runTilemapLua(L,
        "local grid = Grid(tmScene, TM_ENTITY)\n"
        "grid.cellSize = Vector2(8, 4)\n"
        "local x, y = grid:worldToCell(Vector3(17, 5, 0))\n"
        "assert(x == 2 and y == 1, x .. ',' .. y)\n"), "");
    ASSERT_NE(scene.findComponent<GridComponent>(entity), nullptr);
    EXPECT_EQ(scene.getComponent<GridComponent>(entity).cellSize, Vector2(8.0f, 4.0f));
}

// R79 region_too_large: past 1,000,000 cells fill writes nothing and says why; a rectangle over
// the whole int32 range is refused at once (its cell count does not wrap)
TEST(LuaTilemap, FillPastTheLimitIsATypedError){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    TilemapLayer layer(&scene);
    TilemapLuaGlobals globals(L, &scene);
    globals.entity("TM_LAYER", layer.getEntity());
    EXPECT_EQ(runTilemapLua(L, "TilemapLayer(tmScene, TM_LAYER):fill(0, 0, 999, 999, 1)\n"), "");
    EXPECT_EQ(layer.getCellCount(), 1000000);
    layer.clear();

    std::string error = runTilemapLua(L, "TilemapLayer(tmScene, TM_LAYER):fill(0, 0, 1000, 999, 1)\n");
    EXPECT_NE(error.find("region_too_large"), std::string::npos) << error;
    EXPECT_NE(error.find("1001 x 1000"), std::string::npos) << error;
    EXPECT_EQ(layer.getCellCount(), 0);

    error = runTilemapLua(L, "TilemapLayer(tmScene, TM_LAYER):fill(-2147483648, -2147483648, 2147483647, 2147483647, 1)\n");
    EXPECT_NE(error.find("region_too_large"), std::string::npos) << error;
    EXPECT_EQ(layer.getCellCount(), 0);
    // the C++ call refuses the same rectangle
    EXPECT_FALSE(layer.fill(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, 1));
    EXPECT_EQ(layer.getCellCount(), 0);
}

// a TilemapLayer or Grid wrapper of a GameObject that lacks the component (never had it, or it was
// removed): getComponent throws std::out_of_range, which must not unwind through Lua's C frames
TEST(LuaTilemap, CallsWithoutTheComponentAreLuaErrors){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    const Entity plain = tilemapPlainEntity(scene);
    TilemapLuaGlobals globals(L, &scene);
    globals.entity("TM_ENTITY", plain);
    for (const char* call : {"setCell(0, 0, 1)", "getCell(0, 0)", "fill(0, 0, 1, 1, 1)", "worldToCell(Vector3(0, 0, 0))",
                             "cellToWorld(0, 0)", "getCellAt(Vector3(0, 0, 0))", "setTerrain(0, 0, 0)", "refreshAutotile(0, 0, 1, 1)"}){
        const std::string error = runTilemapLua(L, std::string("TilemapLayer(tmScene, TM_ENTITY):") + call + "\n");
        EXPECT_NE(error.find("has no Tilemap Layer"), std::string::npos) << call << ": " << error;
    }
    // the dot instead of the colon: no layer at argument 1
    std::string error = runTilemapLua(L, "local layer = TilemapLayer(tmScene)\nlayer.setCell(0, 0, 1)\n");
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.find("C++ exception"), std::string::npos) << error;

    scene.addComponent<GridComponent>(plain, GridComponent());
    error = runTilemapLua(L, "TM_GRID = Grid(tmScene, TM_ENTITY)\n");
    ASSERT_EQ(error, "");
    scene.removeComponent<GridComponent>(plain);
    error = runTilemapLua(L, "TM_GRID:worldToCell(Vector3(0, 0, 0))\n");
    EXPECT_NE(error.find("has no Grid"), std::string::npos) << error;
    lua_pushnil(L); lua_setglobal(L, "TM_GRID");
    // the Lua state still works
    EXPECT_EQ(runTilemapLua(L, "assert(1 + 1 == 2)\n"), "");
}
