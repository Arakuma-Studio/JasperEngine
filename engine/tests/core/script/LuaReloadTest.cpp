// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua hot reload during Play (S23 R1-R9): the swap keeps the instance tables, their fields and
// every reference to them; a broken file leaves the old code running; named event
// registrations follow the new methods, anonymous callbacks of the old code are dropped.

#include "script/LuaReload.h"
#include "script/LuaBinding.h"
#include "util/Scheduler.h"
#include "component/ScriptComponent.h"
#include "Engine.h"
#include "Log.h"
#include "log/Logger.h"
#include "support/AllocationCounter.h"
#include "LogCapture.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class LuaReloadTest : public ::testing::Test {
    protected:
        lua_State* L = nullptr;
        std::deque<ScriptEntry> entries;   // stable addresses

        void SetUp() override {
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
        }

        void TearDown() override {
            for (ScriptEntry& entry : entries) {
                if (!entry.instance) continue;
                const int ref = static_cast<int>(reinterpret_cast<intptr_t>(entry.instance));
                LuaBinding::removeScriptSubscriptions(ref);
                LuaBinding::releaseLuaRef(ref);
                entry.instance = nullptr;
            }
            lua("reloadHolder = nil; reloadLog = nil; collectgarbage()");
        }

        std::string lua(const std::string& code) {
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        lua_Number number(const std::string& expression) {
            EXPECT_EQ(lua("reloadResult = (" + expression + ")"), "");
            lua_getglobal(L, "reloadResult");
            const lua_Number value = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        bool truth(const std::string& expression) {
            EXPECT_EQ(lua("reloadResult = (" + expression + ")"), "");
            lua_getglobal(L, "reloadResult");
            const bool value = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return value;
        }

        static ScriptProperty property(const std::string& name, ScriptPropertyType type, ScriptPropertyValue value) {
            ScriptProperty prop;
            prop.name = name;
            prop.displayName = name;
            prop.type = type;
            prop.value = value;
            prop.defaultValue = value;
            return prop;
        }

        // An instance as LuaBinding::createLuaScriptInstance makes it: a table of its own whose
        // metatable __index is the module the chunk returned, its properties as fields, then
        // init(). The instance is also global `name` so the test can reach it.
        ScriptEntry& instantiate(const std::string& path, const std::string& code, const std::vector<ScriptProperty>& props,
                                 const std::string& name) {
            entries.emplace_back();
            ScriptEntry& entry = entries.back();
            entry.type = ScriptType::LUA;
            entry.path = path;
            entry.className = "Script";
            entry.enabled = true;
            entry.properties = props;

            const std::string chunk = lua_reload::chunkName(path);
            EXPECT_EQ(luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()), LUA_OK);
            LuaBinding::setScriptChunk(L, path, -1);
            EXPECT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
            lua_newtable(L);
            lua_newtable(L);
            lua_pushvalue(L, -3);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);
            for (const ScriptProperty& prop : props) {
                LuaBinding::pushScriptPropertyValue(L, nullptr, prop);
                lua_setfield(L, -2, prop.name.c_str());
            }
            lua_pushvalue(L, -1);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
            lua_pushinteger(L, ref);
            lua_setfield(L, -2, "__scriptRef");
            for (ScriptProperty& prop : entry.properties) prop.luaRef = ref;
            lua_pushvalue(L, -1);
            lua_setglobal(L, name.c_str());
            lua_getfield(L, -1, "init");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);
                EXPECT_EQ(lua_pcall(L, 1, 0, 0), LUA_OK) << lua_tostring(L, -1);
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
            return entry;
        }

        std::vector<ScriptInstanceRef> refs() {
            std::vector<ScriptInstanceRef> out;
            for (ScriptEntry& entry : entries) out.push_back({nullptr, NULL_ENTITY, &entry});
            return out;
        }
    };

    const char* const COUNTER_V1 = R"(
local Counter = { properties = { { name = "count", type = "int", default = 0 } } }
function Counter:step() self.count = self.count + 1 end
function Counter:kind() return "old" end
return Counter
)";

    const char* const COUNTER_V2 = R"(
local Counter = { properties = { { name = "count", type = "int", default = 0 } } }
function Counter:step() self.count = self.count + 10 end
function Counter:kind() return "new" end
return Counter
)";

}

TEST_F(LuaReloadTest, KeepsInstanceFieldsAndIdentity) {
    ScriptEntry& entry = instantiate("scripts/counter.lua", COUNTER_V1, {property("count", ScriptPropertyType::Int, 0)}, "reloadCounter");
    ASSERT_EQ(lua("reloadCounter.count = 7; reloadCounter.extra = 'kept'; reloadHolder = { target = reloadCounter }"), "");

    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/counter.lua", COUNTER_V2, refs(),
                                                               {property("count", ScriptPropertyType::Int, 0)});
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_EQ(report.instances, 1);
    EXPECT_EQ(number("reloadCounter.count"), 7);
    EXPECT_TRUE(truth("reloadCounter.extra == 'kept'"));
    EXPECT_TRUE(truth("reloadHolder.target == reloadCounter")) << "the holder sees the same table";
    EXPECT_TRUE(truth("reloadCounter:kind() == 'new'"));
    ASSERT_EQ(lua("reloadHolder.target:step()"), "");
    EXPECT_EQ(number("reloadCounter.count"), 17);
    // the entry follows the runtime value (the Inspector during Play)
    ASSERT_EQ(entry.properties.size(), 1u);
    EXPECT_EQ(std::get<int>(entry.properties[0].value), 7) << "read at the swap, before the step";
}

TEST_F(LuaReloadTest, SyntaxErrorKeepsOld) {
    instantiate("scripts/counter.lua", COUNTER_V1, {}, "reloadCounter");
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/counter.lua", "local C = {}\nfunction C:kind( return 1 end\nreturn C", refs(), {});
    EXPECT_TRUE(report.keptOld);
    EXPECT_EQ(report.errorLine, 2);
    EXPECT_EQ(report.error.rfind("scripts/counter.lua:2:", 0), 0u) << report.error;
    EXPECT_TRUE(truth("reloadCounter:kind() == 'old'"));
}

TEST_F(LuaReloadTest, RuntimeErrorAtLoadKeepsOld) {
    instantiate("scripts/counter.lua", COUNTER_V1, {}, "reloadCounter");
    instantiate("scripts/counter.lua", COUNTER_V1, {}, "reloadCounter2");
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/counter.lua",
        "local C = {}\nfunction C:kind() return 'new' end\nlocal broken = nil\nbroken.field = 1\nreturn C", refs(), {});
    EXPECT_TRUE(report.keptOld);
    EXPECT_EQ(report.errorLine, 4);
    EXPECT_TRUE(truth("reloadCounter:kind() == 'old' and reloadCounter2:kind() == 'old'"));
}

TEST_F(LuaReloadTest, NonTableKeepsOld) {
    instantiate("scripts/counter.lua", COUNTER_V1, {}, "reloadCounter");
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/counter.lua", "return 42", refs(), {});
    EXPECT_TRUE(report.keptOld);
    EXPECT_NE(report.error.find("must return a table"), std::string::npos) << report.error;
    EXPECT_TRUE(truth("reloadCounter:kind() == 'old'"));
}

TEST_F(LuaReloadTest, WithoutInstancesTheProbeStillChecks) {
    const lua_reload::Report broken = lua_reload::reloadScript(L, "scripts/unused.lua", "return nil", {}, {});
    EXPECT_TRUE(broken.keptOld);
    const lua_reload::Report fine = lua_reload::reloadScript(L, "scripts/unused.lua", COUNTER_V2, {}, {});
    EXPECT_FALSE(fine.keptOld) << fine.error;
    EXPECT_EQ(fine.instances, 0);
}

TEST_F(LuaReloadTest, ReplaysNamedRegistrations) {
    const char* v1 = R"(
local S = {}
function S:init()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, Engine.onCanvasChanged, "viewChanged")
    Engine.onUpdate:add("reloadAnonymous", function() reloadLog.anonymous = (reloadLog.anonymous or 0) + 1 end)
end
function S:onUpdate() reloadLog.updates = (reloadLog.updates or 0) + 1 end
function S:viewChanged() end
return S
)";
    const char* v2 = R"(
local S = {}
function S:onUpdate() reloadLog.updates = (reloadLog.updates or 0) + 100 end
return S
)";
    ASSERT_EQ(lua("reloadLog = {}"), "");
    instantiate("scripts/events.lua", v1, {}, "reloadEvents");
    Engine::onUpdate.call();
    EXPECT_EQ(number("reloadLog.updates"), 1);
    EXPECT_EQ(number("reloadLog.anonymous"), 1);

    test::LogCapture log;
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/events.lua", v2, refs(), {});
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_EQ(report.replayedCallbacks, 1);
    EXPECT_EQ(report.droppedCallbacks, 1) << "the anonymous Engine.onUpdate:add closure";
    ASSERT_EQ(report.missingMethods.size(), 1u);
    EXPECT_EQ(report.missingMethods[0], "viewChanged");
    EXPECT_EQ(log.count(D_LOG_WARN, "viewChanged"), 1u) << "a removed method warns once";

    Engine::onUpdate.call();
    EXPECT_EQ(number("reloadLog.updates"), 101) << "the registration calls the new method";
    EXPECT_EQ(number("reloadLog.anonymous"), 1) << "the old code's anonymous closure no longer runs";
    Engine::onUpdate.remove("reloadAnonymous");
}

TEST_F(LuaReloadTest, OnReloadRunsOnceAndInitDoesNot) {
    const char* v1 = R"(
local S = {}
function S:init() reloadLog.inits = (reloadLog.inits or 0) + 1 end
return S
)";
    const char* v2 = R"(
local S = { properties = { { name = "speed", type = "float", default = 2.5 } } }
function S:init() reloadLog.inits = (reloadLog.inits or 0) + 1 end
function S:onReload(info)
    reloadLog.reloads = (reloadLog.reloads or 0) + 1
    reloadLog.path = info.path
    reloadLog.added = info.addedProperties[1]
    reloadLog.dropped = info.droppedCallbacks
end
return S
)";
    ASSERT_EQ(lua("reloadLog = {}"), "");
    instantiate("scripts/hooks.lua", v1, {}, "reloadHooks");
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/hooks.lua", v2, refs(),
                                                               {property("speed", ScriptPropertyType::Float, 2.5f)});
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_EQ(number("reloadLog.inits"), 1);
    EXPECT_EQ(number("reloadLog.reloads"), 1);
    EXPECT_TRUE(truth("reloadLog.path == 'scripts/hooks.lua' and reloadLog.added == 'speed' and reloadLog.dropped == 0"));
    EXPECT_FLOAT_EQ(static_cast<float>(number("reloadHooks.speed")), 2.5f);
}

TEST_F(LuaReloadTest, OnReloadErrorIsLoggedAndTheReloadCounts) {
    instantiate("scripts/hooks.lua", COUNTER_V1, {}, "reloadHooks");
    test::LogCapture log;
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/hooks.lua",
        "local S = {}\nfunction S:kind() return 'new' end\nfunction S:onReload() error('boom') end\nreturn S", refs(), {});
    EXPECT_FALSE(report.keptOld);
    EXPECT_TRUE(truth("reloadHooks:kind() == 'new'"));
    Logger::flush();
    EXPECT_TRUE(log.contains("boom"));
}

TEST_F(LuaReloadTest, PropertyRemap) {
    const char* v1 = "local S = {}\nreturn S";
    instantiate("scripts/props.lua", v1,
                {property("kept", ScriptPropertyType::Float, 1.0f), property("speed", ScriptPropertyType::Int, 3),
                 property("label", ScriptPropertyType::String, std::string("a")), property("gone", ScriptPropertyType::Bool, true)},
                "reloadProps");
    ASSERT_EQ(lua("reloadProps.kept = 4.5; reloadProps.speed = 6; reloadProps.label = 'runtime'"), "");

    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/props.lua", v1, refs(), {
        property("kept", ScriptPropertyType::Float, 1.0f),
        property("speed", ScriptPropertyType::Float, 3.0f),            // int -> float: converted
        property("label", ScriptPropertyType::EntityReference, EntityReference{}),  // not convertible: reset
        property("fresh", ScriptPropertyType::Vector3, Vector3(1, 2, 3)),
    });
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_FLOAT_EQ(static_cast<float>(number("reloadProps.kept")), 4.5f) << "same name and kind keep the runtime value";
    EXPECT_TRUE(truth("math.type(reloadProps.speed) == 'float' and reloadProps.speed == 6"));
    EXPECT_TRUE(truth("reloadProps.label == nil"));
    EXPECT_TRUE(truth("reloadProps.fresh.y == 2")) << "a new property gets its default";
    EXPECT_TRUE(truth("reloadProps.gone == nil")) << "a removed property becomes nil";
    EXPECT_EQ(report.converted, std::vector<std::string>({"speed"}));
    EXPECT_EQ(report.reset, std::vector<std::string>({"label"}));
    EXPECT_EQ(report.added, std::vector<std::string>({"fresh"}));
    EXPECT_EQ(report.removed, std::vector<std::string>({"gone"}));
    ASSERT_EQ(entries.front().properties.size(), 4u);
    EXPECT_FLOAT_EQ(std::get<float>(entries.front().properties[1].value), 6.0f);
}

TEST_F(LuaReloadTest, TimersOfTheOldCodeAreDropped) {
    const char* v1 = R"(
local S = {}
function S:init()
    Timer.every(0.001, function() reloadLog.ticks = (reloadLog.ticks or 0) + 1 end)
end
return S
)";
    ASSERT_EQ(lua("reloadLog = {}"), "");
    instantiate("scripts/timers.lua", v1, {}, "reloadTimers");
    Scheduler& scheduler = Scheduler::game();
    scheduler.advance(TimeDomain::Game, 0.001);
    scheduler.process(TimeDomain::Game);
    ASSERT_EQ(number("reloadLog.ticks"), 1);

    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/timers.lua", "return {}", refs(), {});
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_EQ(report.droppedCallbacks, 1);
    scheduler.advance(TimeDomain::Game, 0.001);
    scheduler.process(TimeDomain::Game);
    EXPECT_EQ(number("reloadLog.ticks"), 1);
}

TEST_F(LuaReloadTest, ModulePatchedInPlace) {
    ASSERT_EQ(lua(R"(
-- (the engine's searchers replace package.preload: the module is in package.loaded, as
-- require leaves it)
package.loaded["reloadutil"] = (function()
    local M = { counter = 5 }
    function M.greet() return "old" end
    function M.removed() return true end
    return M
end)()
local util = require("reloadutil")
util.counter = 9
reloadHolder = { util = util }
)"), "");
    const lua_reload::Report report = lua_reload::reloadModule(L, "reloadutil", "lua/reloadutil.lua", R"(
local M = { counter = 0, version = 2 }
function M.greet() return "new" end
function M.added() return 1 end
return M
)");
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_TRUE(truth("reloadHolder.util.greet() == 'new'")) << "a holder of the table sees the new function";
    EXPECT_TRUE(truth("reloadHolder.util.counter == 9")) << "existing data keys keep their values";
    EXPECT_TRUE(truth("reloadHolder.util.version == 2 and reloadHolder.util.added() == 1"));
    EXPECT_TRUE(truth("reloadHolder.util.removed == nil"));
    EXPECT_TRUE(truth("require('reloadutil') == reloadHolder.util"));
    EXPECT_EQ(report.removed, std::vector<std::string>({"removed"}));
    ASSERT_EQ(lua("package.loaded['reloadutil'] = nil"), "");
}

TEST_F(LuaReloadTest, ModuleSyntaxErrorKeepsOld) {
    ASSERT_EQ(lua("package.loaded['reloadutil2'] = { greet = function() return 'old' end }; reloadHolder = package.loaded['reloadutil2']"), "");
    const lua_reload::Report report = lua_reload::reloadModule(L, "reloadutil2", "lua/reloadutil2.lua", "return {");
    EXPECT_TRUE(report.keptOld);
    EXPECT_EQ(report.errorLine, 1);
    EXPECT_TRUE(truth("reloadHolder.greet() == 'old'"));
    ASSERT_EQ(lua("package.loaded['reloadutil2'] = nil"), "");
}

TEST(LuaReloadErrorLine, ReadsBothChunkForms) {
    EXPECT_EQ(lua_reload::errorLine("scripts/p.lua:12: attempt to index a nil value", "scripts/p.lua"), 12);
    EXPECT_EQ(lua_reload::errorLine("[string \"scripts/p.lua\"]:7: unexpected symbol", "scripts/p.lua"), 7);
    EXPECT_EQ(lua_reload::errorLine("other.lua:3: x", "scripts/p.lua"), 0);
    EXPECT_EQ(lua_reload::chunkName("scripts/p.lua"), "@scripts/p.lua");
}

TEST_F(LuaReloadTest, Bench200Instances) {
    const std::string code = std::string(COUNTER_V1);
    for (int i = 0; i < 200; i++) {
        instantiate("scripts/bench.lua", code, {property("count", ScriptPropertyType::Int, 0)}, "reloadBench");
    }
    const auto start = std::chrono::steady_clock::now();
    const lua_reload::Report report = lua_reload::reloadScript(L, "scripts/bench.lua", COUNTER_V2, refs(),
                                                               {property("count", ScriptPropertyType::Int, 0)});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_FALSE(report.keptOld) << report.error;
    EXPECT_EQ(report.instances, 200);
    // R33: 2 ms + 15 us per instance on the main thread; AC7: <= 5 ms on the CI runner
    std::printf("[bench] Lua reload of 200 instances: %.3f ms (budget 5)\n", ms);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ms, 5.0 * testsupport::perfSlack());
    }
    ASSERT_EQ(lua("reloadBench = nil"), "");
}
