// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's dialogue API (docs/specs/dialogue-graphs.md R27, AC8): every member of the hand-written
// stub exists, a runner's events and choose work from Lua, and a handler's "wait" pauses it.

#include "Engine.h"
#include "Scene.h"
#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueVariables.h"
#include "object/DialogueRunner.h"
#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"
#include "script/LuaBinding.h"

#include "TestPaths.h"
#include "support/TestTempDir.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace DialogueLuaTest_cpp {

using namespace jasperengine;

namespace {

    std::string runLua(lua_State* L, const std::string& code) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    class DialogueLua : public ::testing::Test {
    protected:
        void SetUp() override {
            dialogue::registerDialogueKind();
            dialogue::Variables::reset();
            dialogue::Handlers::clear();
        }
        void TearDown() override {
            Engine::clearAllSubscriptions(false);
            dialogue::Variables::reset();
            runLua(LuaBinding::getLuaState(), "dialogueEvents = nil; dialogueRunner = nil");
        }
    };

}

TEST_F(DialogueLua, ApiMatchesStubs) {
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    std::ifstream file(test::testPaths().sourceDir + "/../../core/script/binding/stubs/Dialogue.lua");
    ASSERT_TRUE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string stub = text.str();
    std::set<std::string> functions, variables;
    const std::regex function(R"(function Dialogue\.(\w+)\()");
    for (std::sregex_iterator it(stub.begin(), stub.end(), function), end; it != end; ++it) functions.insert((*it)[1]);
    const std::regex variable(R"(function DialogueVariables\.(\w+)\()");
    for (std::sregex_iterator it(stub.begin(), stub.end(), variable), end; it != end; ++it) variables.insert((*it)[1]);
    EXPECT_EQ(functions, (std::set<std::string>{"onAction", "onCondition", "removeHandler"}));
    EXPECT_EQ(variables, (std::set<std::string>{"get", "set", "toTable", "fromTable", "resetVisits", "save", "load"}));
    for (const std::string& name : functions) { EXPECT_EQ(runLua(L, "assert(type(Dialogue." + name + ") == 'function', '" + name + "')"), "") << name; }
    for (const std::string& name : variables) {
        EXPECT_EQ(runLua(L, "assert(type(Dialogue.variables." + name + ") == 'function', '" + name + "')"), "") << name;
    }
    // and the other way: the Lua tables have nothing the stub does not describe
    EXPECT_EQ(runLua(L,
        "local n = 0 for k in pairs(Dialogue) do n = n + 1 end assert(n == 4, n)\n"
        "local v = 0 for k in pairs(Dialogue.variables) do v = v + 1 end assert(v == 7, v)\n"
        "Dialogue.variables.set('gold', 3)\n"
        "assert(Dialogue.variables.get('gold') == 3)\n"
        "local saved = Dialogue.variables.toTable()\n"
        "assert(saved.variables.gold == 3)\n"
        "Dialogue.variables.set('gold', 4)\n"
        "Dialogue.variables.fromTable(saved)\n"
        "assert(Dialogue.variables.get('gold') == 3)\n"
        "assert(not pcall(function() Dialogue.variables.fromTable({variables = {gold = {}}}) end))\n"
        "local heard = nil\n"
        "Dialogue.variables.onChanged = function(name, value) heard = name .. '=' .. tostring(value) end\n"
        "Dialogue.variables.set('met', true)\n"
        "assert(heard == 'met=true', heard)\n"
        "assert(not pcall(function() Dialogue.variables.x = 1 end))\n"), "");
    // the generated class's members
    EXPECT_EQ(runLua(L,
        "for _, name in ipairs({'start', 'advance', 'choose', 'resume', 'stop', 'setArg'}) do\n"
        "  assert(DialogueRunner[name], name)\n"
        "end\n"), "");
}

TEST_F(DialogueLua, VariablesSaveChangeLoad) {
    test::TestTempDir dir{"dialogue-save-lua"};
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    store.setFolder(dir.path() / "saves");
    lua_State* L = LuaBinding::getLuaState();
    dialogue::Variables::addVisit("guid-a", "n7");
    EXPECT_EQ(runLua(L,
        "Dialogue.variables.set('gold', 3)\n"
        "Dialogue.variables.set('name', 'Ana')\n"
        "assert(Dialogue.variables.save('slot1'))\n"
        "Dialogue.variables.set('gold', 4)\n"
        "Dialogue.variables.set('extra', true)\n"
        "Dialogue.variables.resetVisits()\n"
        "local ok, info = Dialogue.variables.load('slot1')\n"
        "assert(ok, info and info.message)\n"
        "assert(info.namespaces[1] == 'dialogue', info.namespaces[1])\n"
        "assert(Dialogue.variables.get('gold') == 3)\n"
        "assert(Dialogue.variables.get('name') == 'Ana')\n"
        "assert(Dialogue.variables.get('extra') == nil)\n"
        "assert(Dialogue.variables.toTable().visits['guid-a#n7'] == 1)\n"
        // the namespace is SaveGame's: read as any other, or named in the options
        "assert(SaveGame.read('slot1', {namespace = 'dialogue'}).variables.gold == 3)\n"
        "assert(Dialogue.variables.save('slot1', {namespace = 'talk'}))\n"
        "assert(SaveGame.read('slot1', {namespace = 'talk'}).variables.name == 'Ana')\n"
        "assert(SaveGame.write('slot1', {level = 2}))\n"
        "local bad, err = Dialogue.variables.load('slot1', {namespace = 'main'})\n"
        "assert(not bad and err.code == 'corrupt', err and err.message)\n"
        "local missing, err2 = Dialogue.variables.load('slot2')\n"
        "assert(not missing and err2.code == 'not_found', err2 and err2.code)\n"
        "assert(Dialogue.variables.get('gold') == 3)\n"), "");
    // what Lua saved, C++ loads
    dialogue::Variables::reset();
    EXPECT_TRUE(dialogue::SaveHooks::load("slot1").ok());
    EXPECT_TRUE(dialogue::Variables::get("gold").same(dialogue::Value(3.0)));
    EXPECT_EQ(dialogue::Variables::visits("guid-a", "n7"), 1u);
    store.flush();
    jobs::drainMainQueue();
    store.setFolder({});
}

TEST_F(DialogueLua, RunnerEventsAndChoose) {
    lua_State* L = LuaBinding::getLuaState();
    Scene scene;
    const Entity entity = scene.createEntity();
    DialogueRunner runner(&scene, entity);
    runner.setDialogue("dialogue/walk.dialogue");
    luabridge::setGlobal(L, &scene, "dialogueScene");
    luabridge::setGlobal(L, static_cast<int>(entity), "dialogueEntity");
    ASSERT_EQ(runLua(L,
        "dialogueEvents = {}\n"
        "Dialogue.onAction('quest', function(runner, verb, args)\n"
        "  table.insert(dialogueEvents, 'action ' .. verb .. ' ' .. args[1] .. ' ' .. runner.currentNode)\n"
        "end)\n"
        "dialogueRunner = DialogueRunner(dialogueScene, dialogueEntity)\n"
        "dialogueRunner.onStart = function(r) table.insert(dialogueEvents, 'start') end\n"
        "dialogueRunner.onLine = function(r, line)\n"
        "  local text = 'line ' .. line.node .. ' ' .. line.speakerName .. ': ' .. line.text\n"
        "  for _, o in ipairs(line.options) do text = text .. ' [' .. o.index .. ' ' .. o.text .. ' ' .. o.key .. ']' end\n"
        "  table.insert(dialogueEvents, text)\n"
        "end\n"
        "dialogueRunner.onEnd = function(r, result) table.insert(dialogueEvents, 'end ' .. result) end\n"
        "assert(dialogueRunner.state == 'idle')\n"
        "assert(dialogueRunner:start())\n"
        "assert(dialogueRunner.state == 'line' and dialogueRunner.isRunning)\n"
        "assert(dialogueRunner.currentNode == 'n3')\n"
        "assert(dialogueRunner.currentLine.options[2].style == 'exit')\n"
        "assert(dialogueRunner:choose(1))\n"
        "assert(dialogueRunner:advance())\n"
        "assert(dialogueRunner.state == 'ended' and not dialogueRunner.isRunning)\n"), "");
    EXPECT_EQ(runLua(L,
        "local expected = {'start', 'line n3 guard: Halt! You have 10 coins. [1 Pay five walk.n3.o1] [2 Leave walk.n3.o2]',\n"
        "  'action start intro n3', 'line n8 guard: Welcome in. 5 left.', 'end paid'}\n"
        "assert(#dialogueEvents == #expected, #dialogueEvents)\n"
        "for i, e in ipairs(expected) do assert(dialogueEvents[i] == e, dialogueEvents[i]) end\n"), "");
    // the events are the component's: cleared at Stop
    EXPECT_TRUE(scene.getComponent<DialogueRunnerComponent>(entity).onLine.hasSubscribers());
    Engine::clearComponentSubscriptions(&scene);
    EXPECT_FALSE(scene.getComponent<DialogueRunnerComponent>(entity).onLine.hasSubscribers());
}

TEST_F(DialogueLua, WaitReturnValuePauses) {
    lua_State* L = LuaBinding::getLuaState();
    Scene scene;
    const Entity entity = scene.createEntity();
    DialogueRunner(&scene, entity).setDialogue("dialogue/walk.dialogue");
    luabridge::setGlobal(L, &scene, "dialogueScene");
    luabridge::setGlobal(L, static_cast<int>(entity), "dialogueEntity");
    EXPECT_EQ(runLua(L,
        "Dialogue.onAction('quest', function(runner, verb, args) return 'wait' end)\n"
        "dialogueRunner = DialogueRunner(dialogueScene, dialogueEntity)\n"
        "dialogueRunner:start()\n"
        "dialogueRunner:choose(1)\n"
        "assert(dialogueRunner.state == 'waiting', dialogueRunner.state)\n"
        "assert(dialogueRunner.currentNode == 'n5')\n"
        "assert(not dialogueRunner:advance())\n"
        "assert(dialogueRunner:resume())\n"
        "assert(dialogueRunner.state == 'line' and dialogueRunner.currentNode == 'n8')\n"
        "Dialogue.removeHandler('quest')\n"), "");
    EXPECT_EQ(dialogue::Handlers::action("quest"), nullptr);
}

}  // namespace DialogueLuaTest_cpp
