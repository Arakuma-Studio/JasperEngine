// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R30-R32, R73 (AC29, AC31, AC53): the Lua API, end to end through a real state and a real system

#include "VoxelTestSupport.h"

#include "Scene.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/GameObject.h"
#include "object/voxel/VoxelVolume.h"
#include "script/LuaBinding.h"
#include "subsystem/VoxelSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace voxeltest;

namespace {

    class VoxelLua : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::shared_ptr<VoxelSystem> system;
        std::shared_ptr<VoxelAsset> asset;
        Entity owner = NULL_ENTITY;
        lua_State* L = nullptr;

        void SetUp() override {
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("VoxelSystem", true);
            system = scene->getSystem<VoxelSystem>();
            GeneratorDesc generator;
            generator.kind = "solid_ground";
            generator.height = 32.0;
            asset = VoxelAssets::create("mem://lua", desc(64, 64, 64, 2), generator, 4);
            owner = scene->createEntity();
            scene->addComponent<Transform>(owner, {});
            VoxelVolumeComponent component;
            component.volumeData.path = "mem://lua";
            component.collision = false;
            component.materialSlots.resize(4);
            scene->addComponent<VoxelVolumeComponent>(owner, component);
            scene->update(1.0 / 60.0);
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            ASSERT_TRUE(luabridge::push(L, VoxelVolume(scene.get(), owner)));
            lua_setglobal(L, "vol");
        }

        void TearDown() override {
            lua_pushnil(L);
            lua_setglobal(L, "vol");
            lua_gc(L, LUA_GCCOLLECT, 0);
            system.reset();
            scene.reset();
            asset.reset();
        }

        void run(const char* code) {
            ASSERT_EQ(luaL_dostring(L, code), 0) << lua_tostring(L, -1);
        }

        double number(const char* name) {
            lua_getglobal(L, name);
            const double value = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        bool boolean(const char* name) {
            lua_getglobal(L, name);
            const bool value = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return value;
        }

        std::string text(const char* name) {
            lua_getglobal(L, name);
            const char* value = lua_tostring(L, -1);
            std::string out = value ? value : "";
            lua_pop(L, 1);
            return out;
        }
    };

}

TEST_F(VoxelLua, DigFillSmoothPaintReturnReports) {
    run(R"(
        local r = vol:dig(VoxelShape.sphere(Vector3(32, 32, 32), 5), {blend = 0.5})
        digOk = r.ok; digChanged = r.voxelsChanged; digRemoved = r.removedVolume; digBoundsX = r.bounds.minimum.x
        r = vol:fill(VoxelShape.capsule(Vector3(10, 40, 10), Vector3(20, 40, 10), 2), {material = 2})
        fillOk = r.ok; fillAdded = r.addedVolume
        r = vol:smooth(VoxelShape.box(Vector3(32, 32, 32), Vector3(4, 4, 4), Quaternion()), {amount = 0.5, iterations = 2})
        smoothOk = r.ok
        r = vol:paint(VoxelShape.sphere(Vector3(32, 30, 32), 2), {material = 3, weight = 1, falloff = 0.5})
        paintOk = r.ok
        r = vol:paint(VoxelShape.sphere(Vector3(32, 30, 32), 2), {material = 99})
        badOk = r.ok; badError = r.error
    )");
    EXPECT_TRUE(boolean("digOk"));
    EXPECT_GT(number("digChanged"), 0.0);
    EXPECT_GT(number("digRemoved"), 0.0);
    EXPECT_GT(number("digBoundsX"), 10.0);
    EXPECT_TRUE(boolean("fillOk"));
    EXPECT_GT(number("fillAdded"), 0.0);
    EXPECT_TRUE(boolean("smoothOk"));
    EXPECT_TRUE(boolean("paintOk"));
    EXPECT_FALSE(boolean("badOk"));
    EXPECT_EQ(text("badError"), "invalid_material");
}

TEST_F(VoxelLua, QueriesAnswerInMetres) {
    run(R"(
        local hit = vol:raycast(Vector3(32, 60, 32), Vector3(0, -1, 0), 100)
        rayHit = hit.hit; rayY = hit.point.y; rayN = hit.normal.y; rayDistance = hit.distance; raySlot = hit.slot
        above = vol:getValueAt(Vector3(32, 35, 32))
        below = vol:getValueAt(Vector3(32, 29, 32))
        local m = vol:getMaterialAt(Vector3(32, 20, 32))
        slot = m.slot; second = m.second; weight = m.weight
    )");
    EXPECT_TRUE(boolean("rayHit"));
    EXPECT_NEAR(number("rayY"), 32.0, 0.05);
    EXPECT_NEAR(number("rayN"), 1.0, 0.01);
    EXPECT_NEAR(number("rayDistance"), 28.0, 0.05);
    EXPECT_NEAR(number("above"), 3.0, 0.01);
    EXPECT_NEAR(number("below"), -3.0, 0.01);
    EXPECT_EQ(number("slot"), 0.0);
}

TEST_F(VoxelLua, FromGameObjectFollowsAChunkBack) {
    system->finishPendingWork();
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty());
    const VoxelVolume volume = VoxelVolume::fromGameObject(GameObject(scene.get(), chunks[0]));
    EXPECT_EQ(volume.getEntity(), owner);
}

TEST_F(VoxelLua, EventOrderAndDebrisDeterministic) {
    run(R"(
        order = {}
        debris = 0
        vol.onEdited = function(e)
            order[#order + 1] = e.removedVolume > 0 and "dig" or "fill"
            debris = #e.debrisPoints
            removedSlot0 = e.removedByMaterial[1]
            centreY = e.center.y
        end
        vol:dig(VoxelShape.sphere(Vector3(20, 32, 20), 4))
        vol:fill(VoxelShape.sphere(Vector3(40, 40, 40), 3))
    )");
    EXPECT_EQ(number("debris"), 0.0);
    scene->update(1.0 / 60.0);
    run("first = order[1]; second = order[2]; count = #order");
    EXPECT_EQ(number("count"), 2.0);
    EXPECT_EQ(text("first"), "dig");
    EXPECT_EQ(text("second"), "fill");
    run(R"(
        vol.onEdited = function(e)
            debris = #e.debrisPoints
            -- an edit inside a callback applies at once, its event next frame
            if not nested then
                nested = true
                insideOk = vol:dig(VoxelShape.sphere(Vector3(44, 30, 44), 2)).ok
            else
                nestedSeen = true
            end
        end
        vol:dig(VoxelShape.sphere(Vector3(20, 28, 44), 4))
    )");
    scene->update(1.0 / 60.0);
    EXPECT_GT(number("debris"), 0.0);
    EXPECT_LE(number("debris"), 16.0);
    EXPECT_TRUE(boolean("insideOk"));
    EXPECT_FALSE(boolean("nestedSeen"));
    scene->update(1.0 / 60.0);
    EXPECT_TRUE(boolean("nestedSeen"));
}

TEST_F(VoxelLua, RevertChanges) {
    Store* before = system->getStore(owner);
    ASSERT_NE(before, nullptr);
    const uint64_t original = contentHash(*before);
    run("vol:dig(VoxelShape.sphere(Vector3(32, 32, 32), 4)); changed = vol:getValueAt(Vector3(32, 30, 32)) > 0");
    EXPECT_TRUE(boolean("changed"));
    run("vol:revertChanges(); reverted = vol:getValueAt(Vector3(32, 30, 32))");
    EXPECT_NEAR(number("reverted"), -2.0, 0.01);
    EXPECT_EQ(contentHash(*system->getStore(owner)), original);
}

TEST_F(VoxelLua, StatsAndCollisionReadiness) {
    system->finishPendingWork();
    run(R"(
        local s = vol:getStats()
        blocks = s.blocks; memory = s.memoryBytes; chunks = s.chunks
        ready = vol:isCollisionReady(AABB(Vector3(10, 30, 10), Vector3(12, 34, 12)))
    )");
    EXPECT_GT(number("blocks"), 0.0);
    EXPECT_GT(number("memory"), 0.0);
    EXPECT_GT(number("chunks"), 0.0);
    EXPECT_FALSE(boolean("ready"));
}

TEST(VoxelRuntime, PerCallBudget) {
    Store store(desc(128, 128, 128));
    store.setBase(makeSolidGroundBase(64.0, 1.0, 0));
    EditOptions options;
    options.maxVoxels = 256000;
    // radius 20 is a box of 41^3 = 68,921 samples; radius 40 is 531,441
    const EditOp small = digOp(sphereShape(64.0, 64.0, 64.0, 20.0));
    const EditOp big = digOp(sphereShape(64.0, 64.0, 64.0, 40.0));
    const uint64_t before = contentHash(store);
    const EditResult refused = applyEdit(store, std::span<const EditOp>(&big, 1), options);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error, EditError::BudgetExceeded);
    EXPECT_EQ(contentHash(store), before);
    EXPECT_TRUE(applyEdit(store, std::span<const EditOp>(&small, 1), options).ok);
}

TEST(VoxelRuntime, NoOpAllocatesNothing) {
    Store store(desc(64, 64, 64));
    // A dig in air changes nothing: no block, no job, not one allocation
    const EditOp op = digOp(sphereShape(32.0, 32.0, 32.0, 6.0));
    const uint64_t version = store.version();
    {
        testsupport::AllocationCountScope scope;
        const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
        EXPECT_TRUE(result.ok);
        EXPECT_EQ(result.voxelsChanged, 0);
        EXPECT_EQ(scope.count(), 0u);
    }
    EXPECT_EQ(store.version(), version);
}
