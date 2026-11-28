// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The console commands' Lua API (docs/specs/dev-console-for-agents.md AC13): the table form with
// typed arguments by name and position, executeCommand's result table, a Lua error's status and
// traceback, the owner taking the command and its aliases, getCommands' entries.

#include "debug/ConsoleCommands.h"
#include "debug/DebugOverlay.h"
#include "script/LuaBinding.h"
#include "util/FunctionSubscribe.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>

namespace ConsoleCommandsLuaTest_cpp {

using namespace jasperengine;

namespace {

    class ConsoleCommandsLua : public ::testing::Test {
    protected:
        lua_State* L = nullptr;

        void SetUp() override{
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            console::reset();
            console::setCommandsEnabled(true);
        }
        void TearDown() override{
            console::Events::onCheatUsed.clear();
            console::reset();
        }

        // runs a chunk; "" or the error
        std::string run(const std::string& code){
            const int top = lua_gettop(L);
            if (luaL_loadbuffer(L, code.data(), code.size(), "=test") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                const char* message = lua_tostring(L, -1);
                std::string error = message ? message : "(no message)";
                lua_settop(L, top);
                return error;
            }
            lua_settop(L, top);
            return std::string();
        }
    };

}

TEST_F(ConsoleCommandsLua, TableFormTypedArgs){
    ASSERT_EQ(run(R"(
        Seen = {}
        local ok, why = DebugOverlay.addCommand{
            name = "give", description = "adds an item", aliases = {"g"}, cheat = true,
            args = {
                {name = "item", type = "enum", choices = {"gold", "gem", "key"}},
                {name = "count", type = "int", optional = true, default = 1, min = 1, max = 999},
                {name = "where", type = "vector3", optional = true},
            },
            run = function(args, call)
                Seen.item, Seen.count, Seen.first, Seen.second = args.item, args.count, args[1], args[2]
                Seen.where = args.where
                Seen.countType = math.type(args.count)
                Seen.call = call
            end,
        }
        assert(ok, why)
    )"), "");
    console::Result r = console::execute("g gem 7 1,2,3");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(run(R"(
        assert(Seen.item == "gem" and Seen.first == "gem", "by name and by position")
        assert(Seen.count == 7 and Seen.second == 7 and Seen.countType == "integer", "an int is a Lua integer")
        assert(Seen.where.x == 1 and Seen.where.z == 3, "a vector3 is a Vector3")
        local ok, err = pcall(function() Seen.call:print("late") end)
        assert(not ok and tostring(err):find("has ended"), "the call is gone after run")
    )"), "");
    r = console::execute("give gold");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(run(R"(assert(Seen.count == 1 and Seen.where == nil, "the default, and an optional one left out"))"), "");

    // a malformed table is a Lua error; a refused one returns false and the reason
    EXPECT_NE(run(R"(DebugOverlay.addCommand{name = "bad", run = function() end, args = {{name = "x", type = "colour"}}})").find("not string, int"),
              std::string::npos);
    EXPECT_NE(run(R"(DebugOverlay.addCommand{name = "norun"})").find("run must be a function"), std::string::npos);
    EXPECT_EQ(run(R"(
        local ok, why = DebugOverlay.addCommand{name = "grab", aliases = {"g"}, run = function() end}
        assert(ok == false and why == "alias 'g' is taken by 'give'", tostring(why))
    )"), "");
}

TEST_F(ConsoleCommandsLua, ExecuteReturnsResultTable){
    ASSERT_EQ(run(R"(
        Inventory = {gold = 120}
        Cheats = {}
        DebugOverlay.onCheatUsed = function(name) Cheats[#Cheats + 1] = name end
        DebugOverlay.addCommand{
            name = "give", cheat = true,
            args = {{name = "item", type = "enum", choices = {"gold", "gem"}}, {name = "count", type = "int", default = 1}},
            run = function(args, call)
                Inventory[args.item] = (Inventory[args.item] or 0) + args.count
                print("added")
                call:print(("%d %s"):format(args.count, args.item))
                Log.warn("heavy")
                return Inventory[args.item]
            end,
        }
        DebugOverlay.addCommand{name = "inv", run = function() return Inventory end}
        DebugOverlay.addCommand{name = "broke", run = function(args, call) call:fail("no gold") end}
    )"), "");
    EXPECT_EQ(run(R"(
        local r = DebugOverlay.executeCommand("give gold 10")
        assert(r.ok == true and r.status == "ok" and r.command == "give", r.status)
        assert(r.lines[1] == "10 gold" and r.lines[2] == "added", "print first, then the captured lines")
        assert(r.log[1].level == LogLevel.WARN and r.log[1].message:find("heavy"), "the warning")
        assert(r.value == 130 and r.cheat == true, "the returned value")
        assert(DebugOverlay.cheatsUsed == true and Cheats[1] == "give", "the cheat flag and event")
        assert(DebugOverlay.executeCommand("inv").value == Inventory, "Lua gets the value itself")
        local u = DebugOverlay.executeCommand("give ruby")
        assert(u.ok == false and u.status == "usage" and u.usage == "give <item> [count=1]", u.status)
        assert(u.error == 'give: item must be one of gold, gem (got "ruby").', u.error)
        local f = DebugOverlay.executeCommand("broke")
        assert(f.status == "failed" and f.error == "no gold", f.status)
        assert(DebugOverlay.executeCommand("nope").status == "unknown")
        DebugOverlay.resetCheatsUsed()
        assert(DebugOverlay.cheatsUsed == false)
        local c = DebugOverlay.completeCommand("give g", 6)
        assert(c.replacement == "g" and c.candidates[1] == "gem" and c.candidates[2] == "gold" and c.start == 5, c.replacement)
        assert(DebugOverlay.completeCommand("gi").replacement == "give ", "the cursor defaults to the end")
        DebugOverlay.commandsEnabled = false
        assert(DebugOverlay.commandsEnabled == false)
        assert(DebugOverlay.executeCommand("inv").ok, "the game's own calls run with commands off")
    )"), "");
    EXPECT_FALSE(console::commandsEnabled());
}

TEST_F(ConsoleCommandsLua, RunErrorGivesErrorStatusWithTraceback){
    ASSERT_EQ(run(R"(
        DebugOverlay.addCommand{name = "boom", run = function() error("kaboom") end}
        R = DebugOverlay.executeCommand("boom")
    )"), "");
    EXPECT_EQ(run(R"(
        assert(R.status == "error" and R.ok == false, R.status)
        assert(R.error:find("kaboom") and R.error:find("%("), "the message and the first traceback line: " .. R.error)
        assert(not R.error:find("\n"), "one line")
        assert(R.log[1].level == LogLevel.ERROR and R.log[1].message:find("stack traceback"), "the full traceback in log")
    )"), "");
}

TEST_F(ConsoleCommandsLua, OwnerTakesCommandAndAliases){
    ASSERT_EQ(run(R"(
        Owner = {}
        function Owner:init()
            DebugOverlay.addCommand{name = "fly", aliases = {"f"}, run = function() self.flew = true end}
        end
        Owner:init()
    )"), "");
    EXPECT_TRUE(console::execute("f").ok());
    ASSERT_NE(console::find("fly"), nullptr);
    lua_getglobal(L, "Owner");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    LuaBinding::removeScriptSubscriptions(ref);
    LuaBinding::releaseLuaRef(ref);
    EXPECT_EQ(console::find("fly"), nullptr) << "the owner took it";
    EXPECT_EQ(console::find("f"), nullptr) << "and its alias";
}

TEST_F(ConsoleCommandsLua, GetCommandsEntriesKeepNameAndDescription){
    ASSERT_EQ(run(R"(DebugOverlay.addCommand("god", "god mode", function(words) end))"), "");
    EXPECT_EQ(run(R"(
        local found
        for _, cmd in ipairs(DebugOverlay.getCommands()) do
            if cmd.name == "god" then found = cmd end
        end
        assert(found and found.description == "god mode" and found.usage == "god", "name, description, usage")
        assert(found.builtIn == false and found.cheat == false and #found.args == 0)
        assert(found[1] == nil, "entries are tables, not {name, description} pairs")
    )"), "");
}

}  // namespace ConsoleCommandsLuaTest_cpp
