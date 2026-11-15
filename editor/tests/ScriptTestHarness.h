// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Lua scripts on in-memory scenes, stepped by the engine's frame (Engine::systemUpdate): the
// script files are written to a temp directory that is the Lua path of a test System, scripts
// append to a global `log` table the tests read back. For the script lifecycle tests and the
// features built on it (coroutines, persistence).

#pragma once

#include "Engine.h"
#include "System.h"
#include "Scene.h"
#include "component/Transform.h"
#include "component/ScriptComponent.h"
#include "manager/SceneManager.h"
#include "manager/ObjectManager.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "PhysicsTestScene.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include "support/PosixCompat.h"
#include <vector>
#include "log/Logger.h"

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance. The same
    // definition as SpriteNormalsEngineFreeTest.cpp's (one definition across the binary)
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

namespace jasperengine::scripttest {

    // Lua path: the test's directory; the log lines are kept for the tests
    class LuaTestSystem : public System {
    public:
        std::string luaPath;
        std::vector<std::string> logLines;

        int getScreenWidth() override { return 0; }
        int getScreenHeight() override { return 0; }
        std::string getLuaPath() override { return luaPath; }
        std::string getAssetPath() override { return luaPath; }
        void platformLog(const int type, const char* fmt, va_list args) override {
            (void)type;
            char buffer[4096];
            va_list copy;
            va_copy(copy, args);
            std::vsnprintf(buffer, sizeof(buffer), fmt, copy);
            va_end(copy);
            logLines.push_back(buffer);
        }
    };

    class ScriptTest : public ::testing::Test {
    protected:
        std::filesystem::path dir;
        LuaTestSystem system;
        System* previousSystem = nullptr;
        bool gameEventsWereEnabled = true;
        std::vector<Scene*> previousScenes;
        // scenes a test made: their scripts are cleaned up before they go
        std::vector<std::unique_ptr<Scene>> scenes;

        void SetUp() override {
            physicstest::installTestSystem();
            static int counter = 0;
            dir = std::filesystem::temp_directory_path() /
                  ("jasper-script-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            system.luaPath = dir.string();
            previousSystem = SystemTestAccess::swap(&system);
            gameEventsWereEnabled = Engine::onUpdate.isEnabled();
            Engine::pauseGameEvents(false);
            Engine::setFixedDeltatime(1.0 / 60.0);
            previousScenes = Engine::getSceneLayers();
            Engine::removeAllScenes();
            lua("log = {}");
        }

        void TearDown() override {
            Engine::removeAllScenes();
            for (auto& scene : scenes) {
                LuaBinding::cleanupLuaScripts(scene.get());
                ObjectManager::forgetScene(scene.get());
            }
            scenes.clear();
            SceneManager::clearAll();
            lua("log = nil");
            LuaBinding::clearLoadedProjectModules();
            Engine::setFixedDeltatime(0.0);
            Engine::pauseGameEvents(!gameEventsWereEnabled);
            for (size_t i = 0; i < previousScenes.size(); i++) {
                if (i == 0) Engine::setScene(previousScenes[i]);
                else Engine::addSceneLayer(previousScenes[i]);
            }
            ::jasperengine::Logger::flush();   // nothing is delivered to the capture after it goes
            SystemTestAccess::swap(previousSystem);
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        lua_State* L() { return LuaBinding::getLuaState(); }

        Scene* newScene() {
            scenes.push_back(std::make_unique<Scene>());
            return scenes.back().get();
        }

        // Runs Lua code; a returned value comes back as a string ("" for none). Errors fail.
        std::string lua(const std::string& code) {
            lua_State* state = L();
            const int top = lua_gettop(state);
            if (luaL_loadstring(state, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(state, 0, 1) != LUA_OK) {
                ADD_FAILURE() << "Lua: " << lua_tostring(state, -1) << "\nin: " << code;
                lua_settop(state, top);
                return "<error>";
            }
            std::string result;
            if (!lua_isnil(state, -1)) {
                size_t length = 0;
                const char* text = luaL_tolstring(state, -1, &length);
                result.assign(text, length);
                lua_pop(state, 1);
            }
            lua_settop(state, top);
            return result;
        }

        // The error message of code expected to fail ("" if it did not)
        std::string luaError(const std::string& code) {
            lua_State* state = L();
            const int top = lua_gettop(state);
            std::string message;
            if (luaL_loadstring(state, code.c_str()) != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK) {
                message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "?";
            }
            lua_settop(state, top);
            return message;
        }

        // A global for the Lua code of the test
        void setScene(const char* name, Scene* scene) {
            if (!luabridge::push<Scene*>(L(), scene)) lua_pushnil(L());
            lua_setglobal(L(), name);
        }

        std::string log() { return lua("return table.concat(log, ' ')"); }
        void clearLog() { lua("log = {}"); }
        std::string takeLog() {
            std::string text = log();
            clearLog();
            return text;
        }

        void writeScript(const std::string& className, const std::string& code) {
            std::ofstream(dir / (className + ".lua")) << code;
        }

        // A script logging its lifecycle as "<method><entity name><last letter of its class>"
        void writeLogger(const std::string& className, const std::string& extra = "") {
            writeScript(className,
                "local M = {}\n"
                "local function tag(self) return self.scene:getEntityName(self.entity) .. string.sub(self.__name, -1) end\n"
                "M.tag = tag\n"
                "function M:init() log[#log + 1] = 'init' .. tag(self); RegisterEngineEvent(self, 'onUpdate') end\n"
                "function M:onEnable() log[#log + 1] = 'enable' .. tag(self) end\n"
                "function M:start() log[#log + 1] = 'start' .. tag(self) end\n"
                "function M:onUpdate() if not self.updated then self.updated = true; log[#log + 1] = 'update' .. tag(self) end end\n"
                "function M:onDisable() log[#log + 1] = 'disable' .. tag(self) end\n"
                "function M:onDestroy() log[#log + 1] = 'destroy' .. tag(self) end\n"
                "function M:onApplicationQuit() log[#log + 1] = 'quit' .. tag(self) end\n"
                "function M:onApplicationPause(paused) log[#log + 1] = 'pause' .. tostring(paused) .. tag(self) end\n"
                "function M:onApplicationFocus(focused) log[#log + 1] = 'focus' .. tostring(focused) .. tag(self) end\n" +
                extra +
                "return M\n");
        }

        Entity entity(Scene* scene, const std::string& name, Entity parent = NULL_ENTITY, bool active = true) {
            const Entity e = scene->createEntity();
            scene->addComponent<Transform>(e, Transform());
            scene->setEntityName(e, name);
            if (parent != NULL_ENTITY) scene->addEntityChild(parent, e, false);
            if (!active) scene->setEntityActive(e, false);
            return e;
        }

        void addScript(Scene* scene, Entity e, const std::string& className, bool enabled = true) {
            ScriptEntry entry;
            entry.type = ScriptType::LUA;
            entry.path = className + ".lua";
            entry.className = className;
            entry.enabled = enabled;
            if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(e)) {
                scripts->scripts.push_back(entry);
            } else {
                ScriptComponent component;
                component.scripts.push_back(entry);
                scene->addComponent<ScriptComponent>(e, component);
            }
        }

        // One engine frame (start point, fixed steps, update, destroys)
        // (and what it logged reaches system.logLines: the engine's logger delivers on its own thread)
        void frame() { Engine::systemUpdate(); ::jasperengine::Logger::flush(); }

        bool logged(const std::string& part) const {
            ::jasperengine::Logger::flush();   // the engine's logger delivers on its own thread
            for (const std::string& line : system.logLines) {
                if (line.find(part) != std::string::npos) return true;
            }
            return false;
        }
    };

}
