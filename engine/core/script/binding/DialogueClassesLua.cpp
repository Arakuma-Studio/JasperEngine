// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's dialogue API (docs/specs/dialogue-graphs.md R27, R15; docs/dialogue.md): the Dialogue table
// (handlers by prefix, the variable store) bound with the Lua C API (stubs/Dialogue.lua is
// hand-written), and the DialogueRunner class. Without the JASPERENGINE_DIALOGUE module both names
// still exist: any use logs once that the module is not compiled and does nothing.

#include "LuaBinding.h"
#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaFunctionBase.h"

#include "Log.h"

#if JASPERENGINE_DIALOGUE
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueVariables.h"
#include "object/DialogueRunner.h"

#include <cmath>
#include <span>
#include <string>
#include <vector>
#endif

using namespace jasperengine;

namespace {

#if JASPERENGINE_DIALOGUE

    void pushString(lua_State* L, const std::string& text) {
        lua_pushlstring(L, text.data(), text.size());
    }

    void pushValue(lua_State* L, const dialogue::Value& value) {
        if (value.isBool()) lua_pushboolean(L, value.asBool());
        else if (value.isNumber()) {
            const double number = value.asNumber();
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) lua_pushinteger(L, static_cast<lua_Integer>(number));
            else lua_pushnumber(L, number);
        } else if (value.isString()) pushString(L, value.asString());
        else lua_pushnil(L);
    }

    dialogue::Value valueAt(lua_State* L, int index) {
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return dialogue::Value(lua_toboolean(L, index) != 0);
            case LUA_TNUMBER: return dialogue::Value(static_cast<double>(lua_tonumber(L, index)));
            case LUA_TSTRING: {
                size_t length = 0;
                const char* text = lua_tolstring(L, index, &length);
                return dialogue::Value(std::string(text, length));
            }
            default: return dialogue::Value();
        }
    }

    void pushStrings(lua_State* L, const std::vector<std::string>& list) {
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); i++) {
            pushString(L, list[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }

    // R8: {node, key, text, speaker, speakerName, mood, tags, voice, variant, options = {{index, id, key, text, enabled, style, tags}}}
    void pushLine(lua_State* L, const dialogue::Line& line) {
        lua_createtable(L, 0, 10);
        pushString(L, line.node);
        lua_setfield(L, -2, "node");
        pushString(L, line.key);
        lua_setfield(L, -2, "key");
        pushString(L, line.text);
        lua_setfield(L, -2, "text");
        pushString(L, line.speaker);
        lua_setfield(L, -2, "speaker");
        pushString(L, line.speakerName);
        lua_setfield(L, -2, "speakerName");
        pushString(L, line.mood);
        lua_setfield(L, -2, "mood");
        pushStrings(L, line.tags);
        lua_setfield(L, -2, "tags");
        if (!line.voice.path.empty()) {
            pushString(L, line.voice.path);
            lua_setfield(L, -2, "voice");
        }
        if (!line.variant.empty()) {
            pushString(L, line.variant);
            lua_setfield(L, -2, "variant");
        }
        lua_createtable(L, static_cast<int>(line.options.size()), 0);
        for (size_t i = 0; i < line.options.size(); i++) {
            const dialogue::Option& option = line.options[i];
            lua_createtable(L, 0, 7);
            lua_pushinteger(L, option.index);
            lua_setfield(L, -2, "index");
            pushString(L, option.id);
            lua_setfield(L, -2, "id");
            pushString(L, option.key);
            lua_setfield(L, -2, "key");
            pushString(L, option.text);
            lua_setfield(L, -2, "text");
            lua_pushboolean(L, option.enabled);
            lua_setfield(L, -2, "enabled");
            pushString(L, option.style);
            lua_setfield(L, -2, "style");
            pushStrings(L, option.tags);
            lua_setfield(L, -2, "tags");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(L, -2, "options");
    }

    void pushRunner(lua_State* L, dialogue::Runner& runner) {
        Scene* scene = static_cast<Scene*>(runner.getOwnerScene());
        if (!scene || !luabridge::push(L, DialogueRunner(scene, runner.getOwnerEntity()))) lua_pushnil(L);
    }

    // a Lua handler: fn(runner, verb, args); a closed Lua state's function is never called
    class LuaHandler : public LuaFunctionBase {
    public:
        explicit LuaHandler(lua_State* L): LuaFunctionBase(L) {}

        dialogue::Value invoke(dialogue::Runner& runner, std::string_view verb, std::span<const dialogue::Value> args) {
            if (!isLive()) return dialogue::Value();
            push_function(m_vm, m_func);
            pushRunner(m_vm, runner);
            lua_pushlstring(m_vm, verb.data(), verb.size());
            lua_createtable(m_vm, static_cast<int>(args.size()), 0);
            for (size_t i = 0; i < args.size(); i++) {
                pushValue(m_vm, args[i]);
                lua_rawseti(m_vm, -2, static_cast<lua_Integer>(i + 1));
            }
            call(3, 1);
            dialogue::Value result = valueAt(m_vm, -1);
            lua_pop(m_vm, 1);
            return result;
        }
    };

    std::string prefixAt(lua_State* L, int index) {
        std::string prefix = luaL_checkstring(L, index);
        // "quest." and "quest" name the same prefix
        while (!prefix.empty() && prefix.back() == '.') prefix.pop_back();
        if (prefix.empty()) luaL_argerror(L, index, "a prefix (\"quest\")");
        return prefix;
    }

    std::shared_ptr<LuaHandler> handlerAt(lua_State* L, int index) {
        luaL_checktype(L, index, LUA_TFUNCTION);
        lua_pushvalue(L, index);
        return std::make_shared<LuaHandler>(L);
    }

    int luaOnAction(lua_State* L) {
        const std::string prefix = prefixAt(L, 1);
        std::shared_ptr<LuaHandler> handler = handlerAt(L, 2);
        dialogue::Handlers::onAction(prefix, [handler](dialogue::Runner& runner, std::string_view verb, std::span<const dialogue::Value> args) {
            const dialogue::Value result = handler->invoke(runner, verb, args);
            return result.isString() && result.asString() == "wait";
        });
        return 0;
    }

    int luaOnCondition(lua_State* L) {
        const std::string prefix = prefixAt(L, 1);
        std::shared_ptr<LuaHandler> handler = handlerAt(L, 2);
        dialogue::Handlers::onCondition(prefix, [handler](dialogue::Runner& runner, std::string_view verb, std::span<const dialogue::Value> args) {
            return handler->invoke(runner, verb, args);
        });
        return 0;
    }

    int luaRemoveHandler(lua_State* L) {
        dialogue::Handlers::remove(prefixAt(L, 1));
        return 0;
    }

    // ---- Dialogue.variables

    int luaVariablesGet(lua_State* L) {
        pushValue(L, dialogue::Variables::get(luaL_checkstring(L, 1)));
        return 1;
    }

    int luaVariablesSet(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        const int type = lua_type(L, 2);
        if (type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING) luaL_argerror(L, 2, "a bool, number or string");
        dialogue::Variables::set(name, valueAt(L, 2));
        return 0;
    }

    void pushJson(lua_State* L, const nlohmann::ordered_json& json) {
        if (json.is_boolean()) lua_pushboolean(L, json.get<bool>());
        else if (json.is_number_integer() || json.is_number_unsigned()) lua_pushinteger(L, json.get<lua_Integer>());
        else if (json.is_number()) {
            const double number = json.get<double>();
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) lua_pushinteger(L, static_cast<lua_Integer>(number));
            else lua_pushnumber(L, number);
        } else if (json.is_string()) pushString(L, json.get<std::string>());
        else if (json.is_object()) {
            lua_createtable(L, 0, static_cast<int>(json.size()));
            for (auto it = json.begin(); it != json.end(); ++it) {
                pushJson(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
        } else lua_pushnil(L);
    }

    bool jsonAt(lua_State* L, int index, nlohmann::ordered_json& out, int depth = 0) {
        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: out = lua_toboolean(L, index) != 0; return true;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index) && lua_tointeger(L, index) >= 0) out = static_cast<uint64_t>(lua_tointeger(L, index));
                else out = static_cast<double>(lua_tonumber(L, index));
                return true;
            case LUA_TSTRING: out = std::string(lua_tostring(L, index)); return true;
            case LUA_TTABLE: {
                if (depth > 2) return false;
                out = nlohmann::ordered_json::object();
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (lua_type(L, -2) != LUA_TSTRING) {
                        lua_pop(L, 2);
                        return false;
                    }
                    nlohmann::ordered_json value;
                    if (!jsonAt(L, -1, value, depth + 1)) {
                        lua_pop(L, 2);
                        return false;
                    }
                    out[lua_tostring(L, -2)] = std::move(value);
                    lua_pop(L, 1);
                }
                return true;
            }
            default: return false;
        }
    }

    // {variables = {...}, visits = {["<guid>#n7"] = 2}}: what a save keeps
    int luaVariablesToTable(lua_State* L) {
        pushJson(L, dialogue::Variables::toJson());
        return 1;
    }

    int luaVariablesFromTable(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        nlohmann::ordered_json json;
        std::string error;
        if (!jsonAt(L, 1, json) || !dialogue::Variables::fromJson(json, &error)) {
            return luaL_error(L, "Dialogue.variables.fromTable: %s", error.empty() ? "a table toTable() made" : error.c_str());
        }
        return 0;
    }

    int luaVariablesResetVisits(lua_State*) {
        dialogue::Variables::resetVisits();
        return 0;
    }

    int luaVariablesIndex(lua_State* L) {
        const std::string field = luaL_checkstring(L, 2);
        if (field == "onChanged") {
            if (!luabridge::push(L, &dialogue::Variables::onChanged)) lua_pushnil(L);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Dialogue.variables.onChanged = function(name, value) ... end
    int luaVariablesNewIndex(lua_State* L) {
        const std::string field = luaL_checkstring(L, 2);
        if (field == "onChanged") {
            luaL_checktype(L, 3, LUA_TFUNCTION);
            lua_pushvalue(L, 3);
            dialogue::Variables::onChanged = L;
            lua_settop(L, 3);
            return 0;
        }
        return luaL_error(L, "Dialogue.variables.%s is read-only (use set)", field.c_str());
    }

    // {name = value}: start's and setArg's message arguments
    LocalizationArgs argsAt(lua_State* L, int index) {
        LocalizationArgs args;
        if (lua_isnoneornil(L, index)) return args;
        luaL_checktype(L, index, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string name = lua_tostring(L, -2);
                if (lua_isinteger(L, -1)) args.emplace_back(name, static_cast<int64_t>(lua_tointeger(L, -1)));
                else if (lua_type(L, -1) == LUA_TNUMBER) args.emplace_back(name, static_cast<double>(lua_tonumber(L, -1)));
                else if (lua_type(L, -1) == LUA_TBOOLEAN) args.emplace_back(name, std::string(lua_toboolean(L, -1) ? "true" : "false"));
                else if (const char* text = lua_tostring(L, -1)) args.emplace_back(name, std::string(text));
            }
            lua_pop(L, 1);
        }
        return args;
    }

    LocalizationValue argumentAt(lua_State* L, int index) {
        if (lua_isinteger(L, index)) return static_cast<int64_t>(lua_tointeger(L, index));
        if (lua_type(L, index) == LUA_TNUMBER) return static_cast<double>(lua_tonumber(L, index));
        if (lua_type(L, index) == LUA_TBOOLEAN) return std::string(lua_toboolean(L, index) ? "true" : "false");
        return std::string(luaL_checkstring(L, index));
    }

    DialogueRunner& runnerSelf(lua_State* L) {
        auto self = luabridge::get<DialogueRunner*>(L, 1);
        if (!self || !*self) luaL_argerror(L, 1, "a DialogueRunner (call it with ':')");
        return **self;
    }

#endif // JASPERENGINE_DIALOGUE

    // ---- without the module: the names exist, any use logs once

    int notCompiled(lua_State* L) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log::warn("Dialogue: the dialogue module is not compiled into this build (JASPERENGINE_DIALOGUE is off): Dialogue and DialogueRunner do nothing");
        }
        lua_settop(L, 0);
        lua_pushnil(L);
        return 1;
    }

    // a stub: any field is the stub again, a call logs and returns nil
    void pushStub(lua_State* L) {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, notCompiled);
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, [](lua_State* state) -> int {
            lua_pushvalue(state, 1);
            return 1;
        });
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);
    }

}

#if JASPERENGINE_DIALOGUE

// the events' arguments (FunctionSubscribe's Lua callbacks)
void LuaFunctionBase::push_value(lua_State* vm, const DialogueRunner& o) {
    if (!luabridge::push(vm, o)) lua_pushnil(vm);
}

void LuaFunctionBase::push_value(lua_State* vm, const dialogue::Line& o) {
    pushLine(vm, o);
}

void LuaFunctionBase::push_value(lua_State* vm, const dialogue::Value& o) {
    pushValue(vm, o);
}

#endif

void LuaBinding::registerDialogueStubs(lua_State* L) {
#ifndef DISABLE_LUA_BINDINGS
    pushStub(L);
    lua_setglobal(L, "Dialogue");
    pushStub(L);
    lua_setglobal(L, "DialogueRunner");
#endif
}

void LuaBinding::registerDialogueClasses(lua_State* L) {
#ifndef DISABLE_LUA_BINDINGS
#if JASPERENGINE_DIALOGUE
    static const luaL_Reg functions[] = {
        {"onAction", luaOnAction},
        {"onCondition", luaOnCondition},
        {"removeHandler", luaRemoveHandler},
        {nullptr, nullptr},
    };
    static const luaL_Reg variableFunctions[] = {
        {"get", luaVariablesGet},
        {"set", luaVariablesSet},
        {"toTable", luaVariablesToTable},
        {"fromTable", luaVariablesFromTable},
        {"resetVisits", luaVariablesResetVisits},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    luaL_newlib(L, variableFunctions);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, luaVariablesIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaVariablesNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "variables");
    lua_setglobal(L, "Dialogue");

    using RunnerEvent = FunctionSubscribe<void(DialogueRunner)>;
    using LineEvent = FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>;
    using EndEvent = FunctionSubscribe<void(DialogueRunner, const std::string&)>;
    using VariableEvent = FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>;
    using ChangedEvent = FunctionSubscribe<void(const std::string&, const dialogue::Value&)>;
    luabridge::getGlobalNamespace(L)
        .beginClass<RunnerEvent>("FunctionSubscribe_V_DialogueRunner")
        .addFunction("add", (bool (RunnerEvent::*)(const std::string&, lua_State*))&RunnerEvent::add)
        .addFunction("remove", &RunnerEvent::remove)
        .endClass()
        .beginClass<LineEvent>("FunctionSubscribe_V_DialogueLine")
        .addFunction("add", (bool (LineEvent::*)(const std::string&, lua_State*))&LineEvent::add)
        .addFunction("remove", &LineEvent::remove)
        .endClass()
        .beginClass<EndEvent>("FunctionSubscribe_V_DialogueEnd")
        .addFunction("add", (bool (EndEvent::*)(const std::string&, lua_State*))&EndEvent::add)
        .addFunction("remove", &EndEvent::remove)
        .endClass()
        .beginClass<VariableEvent>("FunctionSubscribe_V_DialogueVariable")
        .addFunction("add", (bool (VariableEvent::*)(const std::string&, lua_State*))&VariableEvent::add)
        .addFunction("remove", &VariableEvent::remove)
        .endClass()
        .beginClass<ChangedEvent>("FunctionSubscribe_V_DialogueChanged")
        .addFunction("add", (bool (ChangedEvent::*)(const std::string&, lua_State*))&ChangedEvent::add)
        .addFunction("remove", &ChangedEvent::remove)
        .endClass();

    // Plays a .dialogue on its GameObject (adds a Dialogue Runner when it has none); every line
    // reaches onLine, choose(i) picks among line.options, advance() goes on from a line without choices
    luabridge::getGlobalNamespace(L)
        .deriveClass<DialogueRunner, EntityHandle>("DialogueRunner")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("start", +[](lua_State* state) -> int {
            DialogueRunner& self = runnerSelf(state);
            const std::string entry = luaL_optstring(state, 2, "");
            const LocalizationArgs args = argsAt(state, 3);
            lua_pushboolean(state, self.start(entry, args));
            return 1;
        })
        .addFunction("advance", &DialogueRunner::advance)
        .addFunction("choose", &DialogueRunner::choose)
        .addFunction("resume", &DialogueRunner::resume)
        .addFunction("stop", &DialogueRunner::stop)
        .addFunction("setArg", +[](lua_State* state) -> int {
            DialogueRunner& self = runnerSelf(state);
            self.setArg(luaL_checkstring(state, 2), argumentAt(state, 3));
            return 0;
        })
        .addProperty("dialogue", &DialogueRunner::getDialogue, &DialogueRunner::setDialogue)
        .addProperty("startEntry", &DialogueRunner::getStartEntry, &DialogueRunner::setStartEntry)
        .addProperty("autoStart", &DialogueRunner::isAutoStart, &DialogueRunner::setAutoStart)
        .addProperty("seed", &DialogueRunner::getSeed, &DialogueRunner::setSeed)
        .addProperty("state", &DialogueRunner::getState)
        .addProperty("isRunning", &DialogueRunner::isRunning)
        .addProperty("currentNode", &DialogueRunner::getCurrentNode)
        .addProperty("currentLine", +[](const DialogueRunner* self, lua_State* state) {
            const dialogue::Line* line = self->getCurrentLine();
            if (line) pushLine(state, *line);
            else lua_pushnil(state);
            return luabridge::LuaRef::fromStack(state);
        })
        .addProperty("onStart",
            [](DialogueRunner* self, lua_State*) { return &self->getOnStart(); },
            [](DialogueRunner* self, lua_State* state) { self->getOnStart() = state; })
        .addProperty("onLine",
            [](DialogueRunner* self, lua_State*) { return &self->getOnLine(); },
            [](DialogueRunner* self, lua_State* state) { self->getOnLine() = state; })
        .addProperty("onEnd",
            [](DialogueRunner* self, lua_State*) { return &self->getOnEnd(); },
            [](DialogueRunner* self, lua_State* state) { self->getOnEnd() = state; })
        .addProperty("onVariableChanged",
            [](DialogueRunner* self, lua_State*) { return &self->getOnVariableChanged(); },
            [](DialogueRunner* self, lua_State* state) { self->getOnVariableChanged() = state; })
        .endClass();
#else
    registerDialogueStubs(L);
#endif
#endif //DISABLE_LUA_BINDINGS
}
