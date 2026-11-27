// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's SaveGame module (docs/specs/savegame.md R1, R7, R9, R10; AC18): migrations, callbacks and
// their owners, slot name errors.

#include "Scene.h"
#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"
#include "script/LuaBinding.h"

#include "support/TestTempDir.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace jasperengine;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    int saveLuaIsMainThread(lua_State* L){
        lua_pushboolean(L, jobs::isMainThread() ? 1 : 0);
        return 1;
    }

    std::string runSaveLua(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        lua_register(L, "__isMainThread", saveLuaIsMainThread);
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    bool saveLuaGlobalTrue(const char* name){
        lua_State* L = LuaBinding::getLuaState();
        lua_getglobal(L, name);
        const bool value = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return value;
    }

    // SaveGame on a fresh folder; version 1 and no migrations afterwards
    class SaveGameLua : public ::testing::Test {
    protected:
        TestTempDir dir{"savegame-lua"};

        void SetUp() override {
            savegame::SaveGameStore::instance().setFolder(dir.path() / "saves");
            runSaveLua("SaveGame.version = 1");
        }

        void TearDown() override {
            savegame::SaveGameStore::instance().flush();
            jobs::drainMainQueue();
            savegame::SaveGameStore::instance().setFolder({});
            lua_State* L = LuaBinding::getLuaState();
            lua_pushnil(L);
            lua_setfield(L, LUA_REGISTRYINDEX, "jasperengine.savegame.migrations");
            runSaveLua("SaveGame.version = 1");
        }

        void drainFor(std::chrono::milliseconds time){
            const auto end = std::chrono::steady_clock::now() + time;
            while (std::chrono::steady_clock::now() < end){
                jobs::drainMainQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        bool drainUntilTrue(const char* global){
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!saveLuaGlobalTrue(global)){
                if (std::chrono::steady_clock::now() > deadline) return false;
                jobs::drainMainQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }
    };

}

TEST_F(SaveGameLua, MigrationsRunInOrder){
    ASSERT_EQ(runSaveLua(R"(
        assert(SaveGame.write("slot1", {coins = 7}))
        assert(SaveGame.write("slot1", {"sword"}, {namespace = "inventory"}))
        assert(SaveGame.flush())
        SaveGame.unload()
    )"), "");
    const fs::path file = dir.path() / "saves" / "slot1.g2.jsave";
    const auto stamp = fs::last_write_time(file);
    const uintmax_t size = fs::file_size(file);

    ASSERT_EQ(runSaveLua(R"(
        SaveGame.version = 3
        __order = {}
        SaveGame.addMigration(2, function(ns, info)
            table.insert(__order, info.fromVersion)
            ns.quests = ns.quests or {}
            return ns
        end)
        SaveGame.addMigration(1, function(ns, info)
            table.insert(__order, info.fromVersion)
            ns.main.gold = ns.main.coins or 0
            ns.main.coins = nil
            return ns
        end)
        local data, info = SaveGame.read("slot1")
        assert(data, info and info.message)
        assert(data.gold == 7 and data.coins == nil, "both migrations ran")
        assert(info.migratedFrom == 1, "migratedFrom " .. tostring(info.migratedFrom))
        assert(info.version == 3)
        assert(#__order == 2 and __order[1] == 1 and __order[2] == 2, "in order")
        local quests = SaveGame.read("slot1", {namespace = "quests"})
        assert(type(quests) == "table", "the migration's new namespace reads")
        local again, info2 = SaveGame.read("slot1")
        assert(again.gold == 7 and info2.migratedFrom == nil, "migrated once")
    )"), "");
    // clock ticks: gtest would print time points with C++20 chrono formatting, unavailable
    // below macOS 13.3 (the engine's deployment target is 12.0)
    EXPECT_EQ(fs::last_write_time(file).time_since_epoch().count(), stamp.time_since_epoch().count())
        << "the files keep the old version until the next write";
    EXPECT_EQ(fs::file_size(file), size);
    EXPECT_FALSE(fs::exists(dir.path() / "saves" / "slot1.g3.jsave"));

    ASSERT_EQ(runSaveLua(R"(
        assert(SaveGame.write("slot1", {"sword", "shield"}, {namespace = "inventory"}))
        assert(SaveGame.flush())
        SaveGame.unload()
        local backups = SaveGame.getBackups("slot1")
        assert(backups[1].version == 3, "the next write writes every namespace at version 3")
        local data = SaveGame.read("slot1")
        assert(data.gold == 7)
        assert(SaveGame.read("slot1", {namespace = "quests"}))
    )"), "");
}

TEST_F(SaveGameLua, MissingMigrationLeavesTheFiles){
    ASSERT_EQ(runSaveLua(R"(
        SaveGame.version = 2
        assert(SaveGame.write("slot1", {v = 2}))
        assert(SaveGame.flush())
        SaveGame.unload()
        SaveGame.version = 4
        SaveGame.addMigration(3, function(ns) return ns end)
        local data, err = SaveGame.read("slot1")
        assert(data == nil)
        assert(err.code == "migration", err.code)
        __message = err.message
        local ok, werr = SaveGame.write("slot1", {v = 4}, {namespace = "other"})
        assert(ok == false and werr.code == "migration", "a write does not mix versions")
        SaveGame.addMigration(2, function(ns) error("boom") end)
        local _, ferr = SaveGame.read("slot1")
        assert(ferr.code == "migration" and ferr.message:find("boom"), ferr.message)
        SaveGame.addMigration(2, function(ns) end)
        local _, nerr = SaveGame.read("slot1")
        assert(nerr.code == "migration" and nerr.message:find("returned no table"), nerr.message)
    )"), "");
    lua_State* L = LuaBinding::getLuaState();
    lua_getglobal(L, "__message");
    EXPECT_STREQ(lua_tostring(L, -1), "slot \"slot1\" is version 2 and SaveGame.version is 4, but no migration from 2 is "
                                      "registered (SaveGame.addMigration(2, fn))");
    lua_pop(L, 1);
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir.path() / "saves")) names.push_back(entry.path().filename().string());
    EXPECT_EQ(names, std::vector<std::string>{"slot1.g1.jsave"});
}

TEST_F(SaveGameLua, CallbackDroppedWhenOwnerGone){
    auto scene = std::make_unique<Scene>();
    lua_State* L = LuaBinding::getLuaState();
    luabridge::setGlobal(L, scene.get(), "__saveScene");
    ASSERT_EQ(runSaveLua(R"(
        __ownedCalled = false
        __freeCalled = false
        assert(SaveGame.write("slot1", {a = 1}, {owner = __saveScene, onComplete = function() __ownedCalled = true end}))
        assert(SaveGame.write("slot2", {a = 1}, {onComplete = function() __freeCalled = true end}))
        __saveScene = nil
    )"), "");
    scene.reset();
    ASSERT_TRUE(drainUntilTrue("__freeCalled"));
    drainFor(std::chrono::milliseconds(50));
    EXPECT_FALSE(saveLuaGlobalTrue("__ownedCalled")) << "the scene went: its callback is dropped";
    EXPECT_TRUE(fs::exists(dir.path() / "saves" / "slot1.g1.jsave")) << "the write still happens";
}

TEST_F(SaveGameLua, CallbackRunsOnTheMainThread){
    ASSERT_EQ(runSaveLua(R"(
        __done = false
        __onMain = false
        __okArg = nil
        assert(SaveGame.write("slot1", {a = 1}, {onComplete = function(ok, err)
            __done = true
            __onMain = __isMainThread()
            __okArg = ok and err == nil
        end}))
        assert(__done == false, "not inside write")
    )"), "");
    ASSERT_TRUE(drainUntilTrue("__done"));
    EXPECT_TRUE(saveLuaGlobalTrue("__onMain"));
    EXPECT_TRUE(saveLuaGlobalTrue("__okArg"));
}

TEST_F(SaveGameLua, SlotNameErrorSuggests){
    // a named local, not the macro's argument: MSVC 14.44's preprocessor (Visual Studio 2022 17.14) mangles a raw
    // string holding \" in a macro's arguments (C2017, C3688)
    const char* lua = R"(
        local ok, err = SaveGame.write("Slot 1", {})
        assert(ok == false and err.code == "name")
        assert(err.message == "Slot names use a-z, 0-9, '_' and '-' (got \"Slot 1\"); try \"slot-1\".", err.message)
        local data, rerr = SaveGame.read("My Save!")
        assert(data == nil and rerr.code == "name")
        assert(rerr.message:find('try "my-save-"', 1, true), rerr.message)
        local _, eerr = SaveGame.write("slot1", {f = print})
        assert(eerr.code == "encode" and eerr.message == "cannot save a function at $.f" and eerr.path == "$.f")
        local ok2, err2 = pcall(SaveGame.write, 42, {})
        assert(not ok2 and tostring(err2):find("the slot must be a string"), tostring(err2))
        assert(SaveGame.exists("slot1") == false)
        local missing, merr = SaveGame.read("nothing")
        assert(missing == nil and merr.code == "not_found", merr.code)
    )";
    ASSERT_EQ(runSaveLua(lua), "");
}
