// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting in Lua (docs/specs/S52-sprite-instancing.md R37, AC13): SortingLayer.layers,
// nameToId, idToName, getLayerValue; the renderers' sortingLayerId, sortingLayerName and
// sortingOrder (Unity's names); SortingGroup(scene, entity); an unknown layer is a typed error.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SortingGroupComponent.h"
#include "object/Points.h"
#include "object/Sprite.h"
#include "registry/SortingLayers.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    std::string runSortingLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    struct SortingLayersScope {
        SortingLayersScope(){ SortingLayers::setLayers({{3, "Back"}, {0, "Default"}, {7, "Foreground"}}); }
        ~SortingLayersScope(){ SortingLayers::reset(); }
    };

}

TEST(LuaSorting, SortingLayerTable){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    SortingLayersScope layers;
    EXPECT_EQ(runSortingLua(L,
        "assert(#SortingLayer.layers == 3)\n"
        "assert(SortingLayer.layers[1].name == 'Back' and SortingLayer.layers[1].id == 3)\n"
        "assert(SortingLayer.layers[3].value == 2)\n"
        "assert(SortingLayer.nameToId('Foreground') == 7)\n"
        "assert(SortingLayer.nameToId('Nope') == -1)\n"
        "assert(SortingLayer.idToName(0) == 'Default')\n"
        "assert(SortingLayer.getLayerValue(7) == 2)\n"
        "assert(SortingLayer.getLayerValue(0) == 1)\n"), "");
}

TEST(LuaSorting, RendererProperties){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    SortingLayersScope layers;
    Scene scene;
    Sprite sprite(&scene);
    Points points(&scene);
    luabridge::setGlobal(L, &sprite, "sortSprite");
    luabridge::setGlobal(L, &points, "sortPoints");
    EXPECT_EQ(runSortingLua(L,
        "sortSprite.sortingLayerName = 'Foreground'\n"
        "sortSprite.sortingOrder = 5\n"
        "assert(sortSprite.sortingLayerId == 7)\n"
        "assert(sortSprite.sortingLayerName == 'Foreground')\n"
        "assert(SortingLayer.getLayerValue(sortSprite.sortingLayerId) == 2)\n"
        "sortSprite.sortingLayerId = 3\n"
        "assert(sortSprite.sortingLayerName == 'Back')\n"
        "sortPoints.sortingLayerName = 'Back'\n"
        "sortPoints.sortingOrder = -2\n"), "");
    EXPECT_EQ(sprite.getComponent<MeshComponent>().sortingLayer, 3);
    EXPECT_EQ(sprite.getComponent<MeshComponent>().sortingOrder, 5);
    EXPECT_EQ(points.getComponent<PointsComponent>().sortingOrder, -2);
    // a typed error names the fix; the value stays
    const std::string error = runSortingLua(L, "sortSprite.sortingLayerName = 'Nope'\n");
    EXPECT_NE(error.find("unknown_sorting_layer"), std::string::npos) << error;
    EXPECT_NE(error.find("Sorting Layers"), std::string::npos) << error;
    EXPECT_EQ(sprite.getComponent<MeshComponent>().sortingLayer, 3);
    // Order in Layer clamps to the 16 bits of the sort key
    EXPECT_EQ(runSortingLua(L, "sortSprite.sortingOrder = 100000\nassert(sortSprite.sortingOrder == 32767)\n"), "");
    lua_pushnil(L); lua_setglobal(L, "sortSprite");
    lua_pushnil(L); lua_setglobal(L, "sortPoints");
}

TEST(LuaSorting, SortingGroup){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    SortingLayersScope layers;
    Scene scene;
    Sprite sprite(&scene);
    luabridge::setGlobal(L, &scene, "sortScene");
    lua_pushinteger(L, sprite.getEntity());
    lua_setglobal(L, "SORT_ENTITY");
    EXPECT_EQ(runSortingLua(L,
        "local group = SortingGroup(sortScene, SORT_ENTITY)\n"
        "group.sortingLayerName = 'Foreground'\n"
        "group.sortingOrder = 4\n"
        "assert(group.sortingLayerId == 7)\n"
        "assert(group.enabled)\n"
        "group.enabled = false\n"), "");
    ASSERT_TRUE(scene.findComponent<SortingGroupComponent>(sprite.getEntity()));
    const SortingGroupComponent& group = scene.getComponent<SortingGroupComponent>(sprite.getEntity());
    EXPECT_EQ(group.sortingLayer, 7);
    EXPECT_EQ(group.sortingOrder, 4);
    EXPECT_FALSE(group.enabled);
    lua_pushnil(L); lua_setglobal(L, "sortScene");
}
