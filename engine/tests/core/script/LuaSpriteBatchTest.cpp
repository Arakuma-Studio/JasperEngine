// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Sprite Batch in Lua (docs/specs/S52-sprite-instancing.md R12, R37, §6.6, AC13): SpriteBatch(scene,
// entity) on a Sprite, the bulk calls from flat tables (positions in pairs or triples, rotations,
// scales, frames by index or name, colours, FX by slot name), add / set / remove / clear and count;
// a call validates once and fails with a typed error (frame_range, unknown_frame, length, fx_slot),
// changing nothing.

#include "Scene.h"
#include "component/SpriteBatchComponent.h"
#include "object/Sprite.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    std::string runBatchLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    class LuaSpriteBatch : public ::testing::Test {
    protected:
        lua_State* L = nullptr;
        Scene scene;
        Sprite sprite{&scene};

        void SetUp() override{
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            sprite.addFrame("bullet_a", 0.0f, 0.0f, 8.0f, 8.0f);
            sprite.addFrame("bullet_b", 8.0f, 0.0f, 8.0f, 8.0f);
            luabridge::setGlobal(L, &scene, "batchScene");
            lua_pushinteger(L, sprite.getEntity());
            lua_setglobal(L, "BATCH_ENTITY");
            ASSERT_EQ(runBatchLua(L, "batch = SpriteBatch(batchScene, BATCH_ENTITY)\n"), "");
        }

        void TearDown() override{
            runBatchLua(L, "batch = nil\nbatchScene = nil\ncollectgarbage()\n");
        }

        const SpriteBatchState& state(){
            static SpriteBatchState empty;
            const SpriteBatchComponent& component = scene.getComponent<SpriteBatchComponent>(sprite.getEntity());
            return component.state ? *component.state : empty;
        }
    };

}

TEST_F(LuaSpriteBatch, BulkStreams){
    EXPECT_EQ(runBatchLua(L,
        "assert(batch.count == 0)\n"
        "assert(batch.frameCount == 2)\n"
        "local b = batch:frameId('bullet_b')\n"
        "assert(b == 1)\n"
        "batch:setPositions({1, 2, 3, 4, 5, 6})\n"
        "assert(batch.count == 3)\n"
        "batch:setPositions({1, 2, 9, 3, 4, 8}, 3)\n"
        "assert(batch.count == 2)\n"
        "batch:setRotations({0.5, -0.5})\n"
        "batch:setScales({2, 3})\n"
        "batch:setScales({1, 2, 3, 4}, 2)\n"
        "batch:setFrames({'bullet_b', 0})\n"
        "batch:setColors({1, 0, 0, 1, 0, 1, 0, 0.5})\n"
        "batch:setFx('flash', {0.25, 0.75})\n"
        "batch:setFx('palette', {2, 3})\n"), "");
    const SpriteBatchState& s = state();
    ASSERT_EQ(s.count, 2u);
    EXPECT_FLOAT_EQ(s.positions[1].x, 3.0f);
    EXPECT_FLOAT_EQ(s.positions[1].z, 8.0f);
    EXPECT_FLOAT_EQ(s.rotations[1], -0.5f);
    EXPECT_FLOAT_EQ(s.scales[1].y, 4.0f);
    EXPECT_EQ(s.frames[0], 1);
    EXPECT_FLOAT_EQ(s.colors[1].w, 0.5f);
    ASSERT_TRUE(s.hasFx);
    EXPECT_FLOAT_EQ(s.fx[0].y, 0.25f);
    EXPECT_FLOAT_EQ(s.fx[1].x, 3.0f);
    EXPECT_EQ(scene.getComponent<SpriteBatchComponent>(sprite.getEntity()).count, 2u);
}

TEST_F(LuaSpriteBatch, SmallEdits){
    EXPECT_EQ(runBatchLua(L,
        "local i = batch:add(10, 20, 'bullet_b')\n"
        "assert(i == 0)\n"
        "local j = batch:add(30, 40)\n"
        "assert(j == 1 and batch.count == 2)\n"
        "assert(batch:set(1, 7, 8))\n"
        "assert(not batch:set(5, 0, 0))\n"
        "assert(batch:remove(0))\n"
        "assert(batch.count == 1)\n"
        "batch:clear()\n"
        "assert(batch.count == 0)\n"
        "batch.mode = SpriteBatchMode.CLEAR_EACH_FRAME\n"
        "batch.space = SpriteBatchSpace.WORLD\n"
        "batch.capacity = 64\n"
        "batch.sortInstances = true\n"), "");
    const SpriteBatchComponent& component = scene.getComponent<SpriteBatchComponent>(sprite.getEntity());
    EXPECT_EQ(component.mode, SpriteBatchMode::CLEAR_EACH_FRAME);
    EXPECT_EQ(component.space, SpriteBatchSpace::WORLD);
    EXPECT_EQ(component.capacity, 64u);
    EXPECT_TRUE(component.sortInstances);
}

TEST_F(LuaSpriteBatch, TypedErrorsChangeNothing){
    ASSERT_EQ(runBatchLua(L, "batch:setPositions({0, 0, 1, 1})\n"), "");
    std::string error = runBatchLua(L, "batch:frameId('nope')\n");
    EXPECT_NE(error.find("unknown_frame"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setFrames({0, 5})\n");
    EXPECT_NE(error.find("frame_range"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setFrames({'bullet_a', 'nope'})\n");
    EXPECT_NE(error.find("unknown_frame"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setRotations({1})\n");
    EXPECT_NE(error.find("length"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setPositions({1, 2, 3})\n");
    EXPECT_NE(error.find("length"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setFx('glow', {1, 1})\n");
    EXPECT_NE(error.find("fx_slot"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setColors({1, 1, 1})\n");
    EXPECT_NE(error.find("length"), std::string::npos) << error;
    error = runBatchLua(L, "batch:setPositions({1, 'x'})\n");
    EXPECT_NE(error.find("table of numbers"), std::string::npos) << error;
    const SpriteBatchState& s = state();
    EXPECT_EQ(s.count, 2u);
    EXPECT_EQ(s.frames[1], 0);
    EXPECT_FALSE(s.hasFx);
}
