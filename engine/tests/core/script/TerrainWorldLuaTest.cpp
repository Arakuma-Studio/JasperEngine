// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R47, AC40: Lua's TerrainWorld on worlds whose manifest comes from memory: one with no tiles
// (ready at once, nothing to stream) and one with a tile that is never pumped in, so its areas
// stay pending for as long as the test wants.

#if JASPERENGINE_TERRAIN_WORLD

#include "Scene.h"
#include "component/TerrainWorldComponent.h"
#include "object/Terrain.h"
#include "script/LuaBinding.h"
#include "script/ScriptScheduler.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/world/TerrainWorldData.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace jasperengine;

namespace {

    class MemoryWorldSource final : public TerrainWorldSource {
    public:
        explicit MemoryWorldSource(terrain::WorldManifest manifest): data(std::move(manifest)) {}
        bool manifest(terrain::WorldManifest& out, std::string& error) override {
            (void)error;
            out = data;
            return true;
        }
        // no tile file exists: a present tile stays cold until a pump, which these tests never run
        std::string tilePath(terrain::TileCoord tile) const override { return "terrain-world-lua-test-none/" + terrainWorldTileName(tile); }
        std::string overviewPath() const override { return std::string(); }

    private:
        terrain::WorldManifest data;
    };

    enum class WorldData { None, Empty, OneTile };

    // 64 m tiles; OneTile has tile (0, 0), so world X and Z from 0 to 64 past the World's position
    terrain::WorldManifest testManifest(WorldData kind) {
        terrain::WorldManifest manifest;
        manifest.guid = "6a0c1f52-3a7e-4f0e-9b1d-5c2e7d9a4b10";
        manifest.lattice.tileQuads = 64;
        manifest.lattice.spacing = 1.0f;
        manifest.maps.maxHeight = 100.0f;
        if (kind == WorldData::OneTile) manifest.tiles.push_back({terrain::TileCoord(0, 0), 0, 65535});
        return manifest;
    }

    // A scene with a Terrain GameObject, a Terrain World on it unless asked not to, and the Lua
    // globals testScene, testEntity and testTerrain
    struct LuaWorld {
        std::unique_ptr<Scene> scene;
        std::unique_ptr<Terrain> terrain;
        Entity entity = NULL_ENTITY;
        lua_State* L = nullptr;

        explicit LuaWorld(WorldData data, bool addWorld = true) {
            TerrainWorldSystem::Host host;
            host.makeSource = [data](Scene*, Entity, const TerrainWorldComponent&) -> std::shared_ptr<TerrainWorldSource> {
                if (data == WorldData::None) return nullptr;
                return std::make_shared<MemoryWorldSource>(testManifest(data));
            };
            host.playing = [](Scene*) { return true; };
            TerrainWorldSystem::setHost(host);
            ScriptScheduler::clear();
            scene = std::make_unique<Scene>();
            terrain = std::make_unique<Terrain>(scene.get());
            entity = terrain->getEntity();
            if (addWorld) scene->addComponent<TerrainWorldComponent>(entity, TerrainWorldComponent());
            L = LuaBinding::getLuaState();
            luabridge::setGlobal(L, scene.get(), "testScene");
            luabridge::setGlobal(L, entity, "testEntity");
            luabridge::setGlobal(L, terrain.get(), "testTerrain");
            run("function expectError(text, fn)\n"
                "    local ok, err = pcall(fn)\n"
                "    assert(not ok, 'no error, expected: ' .. text)\n"
                "    assert(string.find(tostring(err), text, 1, true), 'expected \"' .. text .. '\" in: ' .. tostring(err))\n"
                "end");
        }

        ~LuaWorld() {
            ScriptScheduler::clear();
            run("testScene = nil; testEntity = nil; testTerrain = nil; expectError = nil; WaitResult = nil; WaitCo = nil; "
                "Fired = nil; collectgarbage('collect')");
            terrain.reset();
            scene.reset();
            TerrainWorldSystem::setHost(TerrainWorldSystem::Host());
        }

        std::string run(const std::string& code) {
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const std::string error = lua_tostring(L, -1);
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        TerrainWorldSystem& system() { return *scene->getSystem<TerrainWorldSystem>(); }

        // a game frame's coroutine resumes, as Engine::runFrame drives them
        void frame() {
            ScriptScheduler::beginFrame(1.0 / 60.0, 1.0 / 60.0);
            ScriptScheduler::updatePhase();
            ScriptScheduler::endOfFramePhase();
        }
    };

}

TEST(TerrainWorldLua, GetTerrainWorldIsNilWithoutTheComponent) {
    LuaWorld world(WorldData::Empty, false);
    EXPECT_EQ(world.run(R"(
        assert(testTerrain:getTerrainWorld() == nil)
        local w = TerrainWorld(testScene, testEntity)
        expectError("has no Terrain World component", function() w:isReady() end)
        expectError("has no Terrain World component", function() w:addFocus(Vector3(0, 0, 0)) end)
        expectError("has no Terrain World component", function() return w.poolSize end)
    )"), "");
}

TEST(TerrainWorldLua, WorldWithoutDataAnswersAsNothingLoaded) {
    LuaWorld world(WorldData::None);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        assert(w ~= nil)
        assert(not w:isReady())
        assert(w:getTileState(1, 1) == "absent")
        local s = w:getSurfaceAt(1, 1)
        assert(not s.present and not s.exact and s.state == "absent")
        assert(not w:isAreaReady(0, 0, 10, "render"))
        local stats = w:getStats()
        assert(stats.running == false and stats.ready == false)
        assert(string.find(stats.error, "no data", 1, true), stats.error)
        expectError("does not run: the Terrain World has no data", function() w:addFocus(Vector3(0, 0, 0)) end)
        expectError("does not run", function() w:onAreaReady(0, 0, 10, "render", function() end) end)
    )"), "");
    EXPECT_EQ(world.system().getRuntime(world.entity), nullptr);
}

TEST(TerrainWorldLua, BadArgumentsRaise) {
    LuaWorld world(WorldData::Empty);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        expectError('level must be "render" or "collision", got "fast"', function() w:isAreaReady(0, 0, 10, "fast") end)
        expectError("radius must be a finite number >= 0", function() w:isAreaReady(0, 0, -1, "render") end)
        expectError("x and z must be finite", function() w:isAreaReady(0 / 0, 0, 1, "render") end)
        expectError("x and z must be finite", function() w:getSurfaceAt(math.huge, 0) end)
        expectError("must lie within", function() w:isAreaReady(1e9, 0, 1, "render") end)
        expectError("must lie within", function() w:isAreaReady(0, 0, 1e9, "collision") end)
        expectError("radiusScale must be a finite number > 0", function() w:addFocus(Vector3(0, 0, 0), 0) end)
        expectError("no focus 12345", function() w:setFocus(12345, Vector3(0, 0, 0)) end)
        expectError("callback must be a function", function() w:onAreaReady(0, 0, 10, "render", 5) end)
        expectError("x, z and radius must be numbers", function() w:onAreaReady(0, "z", 10, "render", function() end) end)
        expectError("timeoutSeconds must be a number", function() w:waitForArea(0, 0, 10, "render", "soon") end)
        expectError("not inside an engine coroutine", function() w:waitForArea(0, 0, 10, "render") end)
        expectError("poolSize: must be a finite number >= 0", function() w.poolSize = -1 end)
        expectError("collisionRadius: must be a finite number >= 0", function() w.collisionRadius = 0 / 0 end)
    )"), "");
}

TEST(TerrainWorldLua, FocusIsAddedMovedAndRemoved) {
    LuaWorld world(WorldData::Empty);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        Focus = w:addFocus(Vector3(110, 0, 20), 2)
        assert(math.type(Focus) == "integer" and Focus > 0)
        w:setFocus(Focus, Vector3(120, 0, 30), Vector3(1, 0, 0))
    )"), "");
    TerrainWorldRuntime* runtime = world.system().getRuntime(world.entity);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->focusCount(), 1);

    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        assert(w:removeFocus(Focus) == true)
        assert(w:removeFocus(Focus) == false)
        Focus = nil
    )"), "");
    EXPECT_EQ(runtime->focusCount(), 0);

    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        for i = 1, 8 do w:addFocus(Vector3(i, 0, 0)) end
        expectError("at most 8 focuses", function() w:addFocus(Vector3(0, 0, 0)) end)
    )"), "");
    EXPECT_EQ(runtime->focusCount(), 8);
}

TEST(TerrainWorldLua, FocusGoesWithTheWorld) {
    LuaWorld world(WorldData::Empty);
    EXPECT_EQ(world.run("testTerrain:getTerrainWorld():addFocus(Vector3(5, 0, 5))"), "");
    ASSERT_NE(world.system().getRuntime(world.entity), nullptr);
    EXPECT_EQ(world.system().getRuntime(world.entity)->focusCount(), 1);

    world.scene->removeComponent<TerrainWorldComponent>(world.entity);
    EXPECT_EQ(world.system().ensureRuntime(world.entity), nullptr);
    EXPECT_EQ(world.run(R"(
        local w = TerrainWorld(testScene, testEntity)
        expectError("has no Terrain World component", function() w:addFocus(Vector3(0, 0, 0)) end)
    )"), "");

    // the World again is a new runtime, with none of the old focuses
    world.scene->addComponent<TerrainWorldComponent>(world.entity, TerrainWorldComponent());
    TerrainWorldRuntime* again = world.system().ensureRuntime(world.entity);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->focusCount(), 0);
}

TEST(TerrainWorldLua, EmptyWorldIsReadyAndItsWaitsEndAtOnce) {
    LuaWorld world(WorldData::Empty);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        assert(w:isReady())
        assert(w:getTileState(3, 3) == "absent")
        assert(w:getSurfaceAt(3, 3).state == "absent")
        assert(w:isAreaReady(0, 0, 100, "collision"))
        local stats = w:getStats()
        assert(stats.running and stats.ready and stats.tiles == 0 and stats.focuses == 0 and stats.tileStates.resident == 0)
        WaitResult = "pending"
        Coroutine.start(function() WaitResult = w:waitForArea(0, 0, 100, "render", 1) end)
        assert(WaitResult == true)
        Fired = false
        w:onAreaReady(0, 0, 100, "render", function() Fired = true end)
        assert(Fired == false)
        w.poolSize = 12
        w.lodDistance = 1.5
        assert(w.poolSize == 12 and w.lodDistance == 1.5)
    )"), "");
    // R18: the callback runs in the update phase
    world.system().update(0.0);
    EXPECT_EQ(world.run("assert(Fired == true)"), "");
    const TerrainWorldComponent& component = world.scene->getComponent<TerrainWorldComponent>(world.entity);
    EXPECT_EQ(component.poolSize, 12);
    EXPECT_EQ(component.lodDistance, 1.5f);
}

TEST(TerrainWorldLua, WaitForAreaYieldsUntilItsTimeout) {
    LuaWorld world(WorldData::OneTile);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        assert(w:getTileState(10, 10) == "cold" or w:getTileState(10, 10) == "queued")
        assert(not w:isAreaReady(10, 10, 5, "render"))
        WaitResult = "pending"
        WaitCo = Coroutine.start(function() WaitResult = w:waitForArea(10, 10, 5, "render", 0) end)
        assert(WaitResult == "pending" and WaitCo.status == "suspended", WaitCo.status)
    )"), "");
    world.frame();
    EXPECT_EQ(world.run("assert(WaitResult == false, tostring(WaitResult)); assert(WaitCo.status == 'finished', WaitCo.status)"), "");
}

TEST(TerrainWorldLua, WaitForAreaEndsFalseWhenTheWorldGoes) {
    LuaWorld world(WorldData::OneTile);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        WaitResult = "pending"
        WaitCo = Coroutine.start(function() WaitResult = w:waitForArea(10, 10, 5, "collision") end)
        assert(WaitResult == "pending")
    )"), "");
    world.frame();
    EXPECT_EQ(world.run("assert(WaitResult == 'pending')"), "");

    world.scene->removeComponent<TerrainWorldComponent>(world.entity);
    EXPECT_EQ(world.system().ensureRuntime(world.entity), nullptr);
    world.frame();
    EXPECT_EQ(world.run("assert(WaitResult == false, tostring(WaitResult))"), "");
}

TEST(TerrainWorldLua, StoppedWaitsOutliveTheirScene) {
    LuaWorld world(WorldData::OneTile);
    EXPECT_EQ(world.run(R"(
        local w = testTerrain:getTerrainWorld()
        WaitCo = Coroutine.start(function() w:waitForArea(10, 10, 5, "render") end)
        WaitCo:stop()
        assert(WaitCo.status == "stopped", WaitCo.status)
        WaitCo = Coroutine.start(function() w:waitForArea(20, 20, 5, "render") end)
        assert(WaitCo.status == "suspended", WaitCo.status)
    )"), "");
    // Play stopped: the scene goes before the coroutines are cleared, which closes the wait
    world.terrain.reset();
    world.scene.reset();
    ScriptScheduler::clear();
    EXPECT_EQ(world.run("collectgarbage('collect')"), "");
}

#endif // JASPERENGINE_TERRAIN_WORLD
