// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC24, AC25: the runtime overlay and its Lua API, without a GPU. The doc's API rows are checked by
// name so the page cannot drift from the bindings.

#include "FoliageTestSupport.h"

#include "TestPaths.h"
#include "object/Terrain.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace foliagetest;

namespace {

    std::string runChunk(lua_State* L, const std::string& code) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    struct LuaFoliage {
        FoliageFixture fixture;
        lua_State* L = LuaBinding::getLuaState();
        Terrain terrain;

        LuaFoliage(std::initializer_list<std::pair<float, float>> points): terrain(fixture.scene.get(), fixture.entity) {
            withTrees(fixture, points);
            fixture.settle();
            luabridge::setGlobal(L, &terrain, "terrain");
        }

        ~LuaFoliage() {
            lua_pushnil(L);
            lua_setglobal(L, "terrain");
            lua_pushnil(L);
            lua_setglobal(L, "result");
        }

        int64_t idOf(int index) {
            const std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
            return static_cast<int64_t>(makeTreeId(0, store->records()[index].id));
        }
    };

}

TEST(FoliageLua, RemoveHidesAndFreesThePath) {
    LuaFoliage lua({{10.0f, 10.0f}, {-20.0f, 5.0f}});
    const int64_t id = lua.idOf(0);
    lua_pushinteger(lua.L, id);
    lua_setglobal(lua.L, "ID");
    ASSERT_EQ(runChunk(lua.L, R"(
        result = terrain:removeTreeInstance(ID)
        assert(result ~= nil and result.prototype == 0)
        assert(terrain:removeTreeInstance(ID) == nil, "a second remove finds nothing")
    )"), "");
    lua.fixture.frame();
    FoliageInstanceInfo pose;
    EXPECT_FALSE(lua.fixture.foliage->getTreePose(lua.fixture.entity, static_cast<TreeId>(id), pose));
    EXPECT_EQ(lua.fixture.foliage->getTreeStore(lua.fixture.entity)->size(), 2u) << "the store keeps it";
}

TEST(FoliageLua, RestoreReturnsIt) {
    LuaFoliage lua({{10.0f, 10.0f}});
    const int64_t id = lua.idOf(0);
    lua_pushinteger(lua.L, id);
    lua_setglobal(lua.L, "ID");
    ASSERT_EQ(runChunk(lua.L, R"(
        terrain:removeTreeInstance(ID)
        assert(terrain:restoreTreeInstance(ID) == true)
        assert(terrain:restoreTreeInstance(ID) == false, "nothing left to restore")
    )"), "");
    FoliageInstanceInfo pose;
    EXPECT_TRUE(lua.fixture.foliage->getTreePose(lua.fixture.entity, static_cast<TreeId>(id), pose));
}

TEST(FoliageLua, StateRoundTrips) {
    LuaFoliage lua({{10.0f, 10.0f}, {-20.0f, 5.0f}});
    lua_pushinteger(lua.L, lua.idOf(0));
    lua_setglobal(lua.L, "ID");
    ASSERT_EQ(runChunk(lua.L, R"(
        terrain:removeTreeInstance(ID)
        added = terrain:addTreeInstance({prototype = 0, position = Vector3(3, 0, 4), yaw = 30, scale = 2})
        assert(added ~= 0)
        terrain:clearDetailsInRadius(Vector3(0, 0, 0), 4)
        state = terrain:getRuntimeFoliageState()
        assert(type(state) == "string" and #state > 0)
        terrain:restoreTreeInstance(ID)
        assert(terrain:setRuntimeFoliageState(state) == true)
        assert(terrain:getRuntimeFoliageState() == state)
        assert(terrain:setRuntimeFoliageState("not a state") == false)
        assert(terrain:getRuntimeFoliageState() == state, "a bad string changes nothing")
        state = nil
        added = nil
    )"), "");
}

TEST(FoliageLua, ReloadDropsTheOverlay) {
    LuaFoliage lua({{10.0f, 10.0f}});
    const uint64_t before = lua.fixture.foliage->getPlacementHash(lua.fixture.entity);
    lua.fixture.foliage->removeTreeInstance(lua.fixture.entity, static_cast<TreeId>(lua.idOf(0)));
    lua.fixture.frame();
    lua.fixture.foliage->waitForJobs();
    lua.fixture.foliage->clearRuntimeState(lua.fixture.entity);
    lua.fixture.settle();
    FoliageInstanceInfo pose;
    EXPECT_TRUE(lua.fixture.foliage->getTreePose(lua.fixture.entity, static_cast<TreeId>(lua.idOf(0)), pose));
    EXPECT_EQ(lua.fixture.foliage->getPlacementHash(lua.fixture.entity), before);
}

TEST(FoliageLua, SettingsReachTheComponent) {
    LuaFoliage lua({{10.0f, 10.0f}});
    ASSERT_EQ(runChunk(lua.L, R"(
        terrain.detailDistance = 60
        terrain.detailDensity = 0.5
        terrain:setDetailDensity(0, 0.25)
        terrain.treeDistance = 400
        terrain.billboardStart = 60
        terrain.fadeLength = 4
        terrain.maxMeshTrees = 100
        terrain.windDirection = Vector3(1, 0, 0)
        terrain.windSpeed = 2
        terrain.windSize = 9
        terrain.windBending = 0.25
    )"), "");
    const TerrainComponent& component = lua.fixture.terrain();
    EXPECT_FLOAT_EQ(component.detailDistance, 60.0f);
    EXPECT_FLOAT_EQ(component.detailDensity, 0.5f);
    EXPECT_FLOAT_EQ(component.foliageLayers[0].densityScale, 0.25f);
    EXPECT_FLOAT_EQ(component.treeDistance, 400.0f);
    EXPECT_FLOAT_EQ(component.billboardStart, 60.0f);
    EXPECT_FLOAT_EQ(component.fadeLength, 4.0f);
    EXPECT_EQ(component.maxMeshTrees, 100);
    EXPECT_FLOAT_EQ(component.windDirection.x, 1.0f);
    EXPECT_FLOAT_EQ(component.windSpeed, 2.0f);
    EXPECT_FLOAT_EQ(component.windSize, 9.0f);
    EXPECT_FLOAT_EQ(component.windBending, 0.25f);
}

// Every terrain:name the page uses is a binding (the Lua block doc test of docs/scripting.md does not
// run this page's chunks: they need a scene with this Terrain)
TEST(FoliageLua, ApiMatchesTheDocs) {
    LuaFoliage lua({{10.0f, 10.0f}});
    const std::filesystem::path page = std::filesystem::path(jasperengine::test::testPaths().sourceDir) / ".." / ".." / ".." / "docs" / "scripting" / "foliage.md";
    std::ifstream in(page);
    ASSERT_TRUE(in.good()) << page;
    std::stringstream text;
    text << in.rdbuf();
    const std::string content = text.str();

    std::set<std::string> names;
    const std::regex use(R"(terrain[:.]([a-zA-Z]+))");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), use); it != std::sregex_iterator(); ++it) names.insert((*it)[1].str());
    EXPECT_GE(names.size(), 15u);
    for (const std::string& name : names) {
        lua_getglobal(lua.L, "terrain");
        lua_getmetatable(lua.L, -1);
        lua_pushstring(lua.L, name.c_str());
        lua_rawget(lua.L, -2);
        bool found = !lua_isnil(lua.L, -1);
        lua_pop(lua.L, 1);
        if (!found) {
            // properties live in the class's getter and setter tables
            const std::string probe = "local _ = terrain." + name + "; terrain." + name + " = terrain." + name;
            found = name.rfind("on", 0) == 0 || runChunk(lua.L, probe).empty();
        }
        lua_pop(lua.L, 2);
        EXPECT_TRUE(found) << "docs/scripting/foliage.md names terrain." << name;
    }
}
