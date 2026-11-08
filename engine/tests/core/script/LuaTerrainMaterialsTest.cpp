// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R48, AC41: the terrain material's Lua surface, from a script, as a game reads and writes it.

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "object/Terrain.h"
#include "script/LuaBinding.h"
#include "terrain/TerrainQuality.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    std::string runLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            const std::string error = lua_tostring(L, -1);
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaTerrainMaterials, EveryMemberReadsAndWrites){
    terrain::TerrainQualitySettings::reset();
    Scene scene;
    Terrain terrainObject(&scene);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    luabridge::setGlobal(L, &terrainObject, "testTerrain");

    const std::string error = runLua(L, R"(
        local t = testTerrain
        t.terrainQuality = TerrainQualitySetting.LOW
        assert(t.terrainQuality == TerrainQualitySetting.LOW)
        assert(t.effectiveQuality == "low")
        t.limitLayersPerPixel = true
        assert(t.limitLayersPerPixel)
        t.offscreenShadows = TerrainOffscreenShadows.OFF
        assert(t.offscreenShadows == TerrainOffscreenShadows.OFF)
        t.macroMode = TerrainMacroMode.NOISE
        assert(t.macroMode == TerrainMacroMode.NOISE)
        t.macroScale = 12
        assert(t.macroScale == 12)
        t.macroStrength = 0.5
        assert(t.macroStrength == 0.5)
        t.resampleEnabled = true
        assert(t.resampleEnabled)
        t.resampleNear = 4
        assert(t.resampleNear == 4)
        t.resampleFar = 64
        assert(t.resampleFar == 64)
        t.heightBlendSharpness = 0.5
        assert(t.heightBlendSharpness == 0.5)
        t.macroNormal = true
        assert(t.macroNormal)
        t:setHoleMap("holes.png")
        assert(t:getHoleMapPath() ~= "")
        t:setHoleMap("")
        assert(t:getHoleMapPath() == "")
        Engine.setTerrainQuality(TerrainQualitySetting.MEDIUM)
        assert(t.effectiveQuality == "medium")
        Engine.clearTerrainQuality()
        assert(t.effectiveQuality == "low")
    )");
    EXPECT_EQ(error, "");

    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrainObject.getEntity());
    EXPECT_EQ(component.quality, TerrainQualitySetting::LOW);
    EXPECT_TRUE(component.macroNormal);
    terrain::TerrainQualitySettings::reset();
}
