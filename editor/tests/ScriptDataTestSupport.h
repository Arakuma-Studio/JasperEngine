// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by LuaJsonTest, UserSettingsTest and FileUtilsTest (feature script-data): a System
// whose asset, Lua and user data folders are a fresh temporary directory, installed for one
// test (the UserSettings store follows it), and the engine's Lua state to run Lua in.
#pragma once

#include "Engine.h"
#include "System.h"
#include "io/UserSettingsStore.h"
#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "support/PosixCompat.h"
#include <vector>

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

namespace headlesssystem {

    // A suite with its own headless project (the editor's Platform as System::instance(), the
    // binary's engine being TestMain's: a second Engine::systemInit would restart its jobs and
    // logger) swaps its System in for each of its tests; the binary's own System is back after
    // every test (its log capture: other suites read it)
    inline jasperengine::System* previousSystem = nullptr;
    inline bool systemSwapped = false;

    struct RestoreSystem : ::testing::EmptyTestEventListener {
        void OnTestEnd(const ::testing::TestInfo&) override {
            if (systemSwapped) {
                jasperengine::SystemTestAccess::swap(previousSystem);
                systemSwapped = false;
            }
        }
    };

    inline void use(jasperengine::System* system) {
        static const bool listening = []() {
            ::testing::UnitTest::GetInstance()->listeners().Append(new RestoreSystem());
            return true;
        }();
        (void)listening;
        if (jasperengine::System::hasInstance() && &jasperengine::System::instance() == system) return;
        jasperengine::System* previous = jasperengine::SystemTestAccess::swap(system);
        if (!systemSwapped) previousSystem = previous;
        systemSwapped = true;
    }

}

namespace scriptdata {

    namespace fs = std::filesystem;

    class TestSystem : public jasperengine::System {
    public:
        std::string root;

        int getScreenWidth() override { return 64; }
        int getScreenHeight() override { return 64; }
        std::string getAssetPath() override { return root + "/assets"; }
        std::string getLuaPath() override { return root + "/lua"; }
        std::string getUserDataPath() override { return root + "/data"; }
        // the web build's IndexedDB restore pending (false): data:// writes are refused
        bool userDataLoaded = true;
        bool isUserDataLoaded() override { return userDataLoaded; }
    };

    // The names of the atomic writes' temporary files left in `dir` (".<name>.<pid>-<n>.tmp")
    inline std::vector<std::string> temporaryFiles(const fs::path& dir) {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) names.push_back(name);
        }
        return names;
    }

    inline std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    inline void writeFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    // The engine's Lua state (another test of this binary may have made it; else it is made
    // here, once, with a System of its own that lives as long as the process)
    inline lua_State* engineLua() {
        if (!jasperengine::LuaBinding::getLuaState()) {
            static TestSystem processSystem;
            if (!jasperengine::System::hasInstance()) {
                processSystem.root = (fs::temp_directory_path() / ("jasper-scriptdata-process-" + std::to_string(::getpid()))).string();
                static char arg0[] = "jasperengine-editor-tests";
                static char* argv[] = {arg0, nullptr};
                jasperengine::Engine::systemInit(1, argv, &processSystem);
                jasperengine::Engine::pauseGameEvents(true);
            }
        }
        return jasperengine::LuaBinding::getLuaState();
    }

    // A fresh folder as the user data, asset and Lua folders of System::instance() for one test
    class ScriptDataTest : public ::testing::Test {
    protected:
        TestSystem system;
        jasperengine::System* previous = nullptr;
        fs::path root;
        lua_State* L = nullptr;

        void SetUp() override {
            L = engineLua();
            ASSERT_NE(L, nullptr);
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("jasper-scriptdata-" + std::to_string(::getpid()) + "-" + info->name());
            fs::remove_all(root);
            fs::create_directories(root / "data");
            fs::create_directories(root / "assets");
            fs::create_directories(root / "lua");
            system.root = root.string();
            previous = jasperengine::SystemTestAccess::swap(&system);
            jasperengine::UserSettingsStore::instance().unload();
        }

        void TearDown() override {
            jasperengine::UserSettingsStore::instance().setClock(nullptr);
            jasperengine::UserSettingsStore::instance().unload();
            jasperengine::SystemTestAccess::swap(previous);
            lua_settop(L, 0);
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // Runs `code` (a chunk; `return` values are kept); "" or the error message
        std::string run(const std::string& code, std::vector<std::string>* results = nullptr) {
            const int top = lua_gettop(L);
            if (luaL_loadbuffer(L, code.data(), code.size(), "=test") != LUA_OK) {
                std::string error = lua_tostring(L, -1);
                lua_settop(L, top);
                return "load: " + error;
            }
            if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
                const char* message = lua_tostring(L, -1);
                std::string error = message ? message : "(no message)";
                lua_settop(L, top);
                return error.empty() ? "(empty error)" : error;
            }
            if (results) {
                results->clear();
                for (int i = top + 1; i <= lua_gettop(L); i++) {
                    size_t n = 0;
                    const char* s = luaL_tolstring(L, i, &n);
                    results->push_back(std::string(s, n));
                    lua_pop(L, 1);
                }
            }
            lua_settop(L, top);
            return "";
        }

        // The first value `code` returns, as tostring gives it ("ERROR: <message>" on an error)
        std::string eval(const std::string& code) {
            std::vector<std::string> results;
            const std::string error = run(code, &results);
            if (!error.empty()) return "ERROR: " + error;
            return results.empty() ? "(nothing)" : results[0];
        }
    };

}
