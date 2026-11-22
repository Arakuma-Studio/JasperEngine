// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's dialogue API (docs/specs/dialogue-graphs.md R27, R15; docs/dialogue.md): the Dialogue table
// (handlers by prefix, the variable store) bound with the Lua C API (stubs/Dialogue.lua is
// hand-written), and the DialogueRunner class. Without the JASPERENGINE_DIALOGUE module both names
// still exist: any use logs once that the module is not compiled and does nothing.

#include "LuaBinding.h"
#include "dialogue/DialogueSwitch.h"
#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaFunctionBase.h"

#include "Log.h"

#if JASPERENGINE_DIALOGUE_ENABLED
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueVariables.h"
#include "object/DialogueRunner.h"

#include <cmath>
#include <span>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#endif

using namespace jasperengine;

namespace {

#if JASPERENGINE_DIALOGUE_ENABLED

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

    // Lua is compiled as C: a raise (luaL_error, luaL_check*) longjmps over the C++ frames up to the
    // pcall, running no destructor (MSVC may end the process). The functions check their arguments
    // while their frame holds nothing with a destructor, and raise only then.

    // the prefix at `index` without its trailing dots ("quest." and "quest" name the same one), a
    // view of the Lua string
    std::string_view prefixAt(lua_State* L, int index) {
        size_t length = 0;
        const char* text = luaL_checklstring(L, index, &length);
        while (length > 0 && text[length - 1] == '.') length--;
        if (length == 0) luaL_argerror(L, index, "a prefix (\"quest\")");
        return std::string_view(text, length);
    }

    // the function at `index` (luaL_checktype'd by the caller)
    std::shared_ptr<LuaHandler> handlerAt(lua_State* L, int index) {
        lua_pushvalue(L, index);
        return std::make_shared<LuaHandler>(L);
    }

    int luaOnAction(lua_State* L) {
        const std::string_view prefixText = prefixAt(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const std::string prefix(prefixText);
        std::shared_ptr<LuaHandler> handler = handlerAt(L, 2);
        dialogue::Handlers::onAction(prefix, [handler](dialogue::Runner& runner, std::string_view verb, std::span<const dialogue::Value> args) {
            const dialogue::Value result = handler->invoke(runner, verb, args);
            return result.isString() && result.asString() == "wait";
        });
        return 0;
    }

    int luaOnCondition(lua_State* L) {
        const std::string_view prefixText = prefixAt(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const std::string prefix(prefixText);
        std::shared_ptr<LuaHandler> handler = handlerAt(L, 2);
        dialogue::Handlers::onCondition(prefix, [handler](dialogue::Runner& runner, std::string_view verb, std::span<const dialogue::Value> args) {
            return handler->invoke(runner, verb, args);
        });
        return 0;
    }

    int luaRemoveHandler(lua_State* L) {
        dialogue::Handlers::remove(std::string(prefixAt(L, 1)));
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

    // the table at 1 into the store; false and why in `message`
    bool variablesFromTable(lua_State* L, char* message, size_t size) {
        nlohmann::ordered_json json;
        std::string error;
        if (jsonAt(L, 1, json) && dialogue::Variables::fromJson(json, &error)) return true;
        std::snprintf(message, size, "%s", error.empty() ? "a table toTable() made" : error.c_str());
        return false;
    }

    int luaVariablesFromTable(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        char message[512];
        if (!variablesFromTable(L, message, sizeof(message))) return luaL_error(L, "Dialogue.variables.fromTable: %s", message);
        return 0;
    }

    // A copy of the options at `index` (nil: none) with namespace "dialogue" unless one is given
    void pushSaveOptions(lua_State* L, int index) {
        lua_newtable(L);
        if (lua_istable(L, index)) {
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_settable(L, -4);
            }
        }
        if (lua_getfield(L, -1, "namespace") == LUA_TNIL) {
            lua_pushstring(L, dialogue::SaveHooks::SAVE_NAMESPACE);
            lua_setfield(L, -3, "namespace");
        }
        lua_pop(L, 1);
    }

    // Through SaveGame.write and SaveGame.read, so a slot's migrations, callbacks and errors are theirs
    bool pushSaveGameFunction(lua_State* L, const char* fn, const char* caller) {
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2)) {
            luaL_error(L, "Dialogue.variables.%s: options must be a table, got %s", caller, luaL_typename(L, 2));
            return false;
        }
        if (lua_getglobal(L, "SaveGame") != LUA_TTABLE || lua_getfield(L, -1, fn) != LUA_TFUNCTION) {
            luaL_error(L, "Dialogue.variables.%s: SaveGame.%s is not available", caller, fn);
            return false;
        }
        lua_remove(L, -2);
        return true;
    }

    int luaVariablesSave(lua_State* L) {
        lua_settop(L, 2);
        if (!pushSaveGameFunction(L, "write", "save")) return 0;
        lua_pushvalue(L, 1);
        pushJson(L, dialogue::Variables::toJson());
        pushSaveOptions(L, 2);
        lua_call(L, 3, LUA_MULTRET);
        return lua_gettop(L) - 2;
    }

    int luaVariablesLoad(lua_State* L) {
        lua_settop(L, 2);
        pushSaveOptions(L, 2);
        const int options = lua_gettop(L);
        if (!pushSaveGameFunction(L, "read", "load")) return 0;
        lua_pushvalue(L, 1);
        lua_pushvalue(L, options);
        lua_call(L, 2, 2);
        const int data = options + 1;
        if (lua_isnil(L, data)) {
            lua_pushboolean(L, 0);
            lua_pushvalue(L, data + 1);
            return 2;
        }
        nlohmann::ordered_json json;
        std::string error;
        if (!jsonAt(L, data, json) || !dialogue::Variables::fromJson(json, &error)) {
            lua_getfield(L, options, "namespace");
            const std::string nameSpace = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            const std::string slot = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
            lua_pushboolean(L, 0);
            lua_createtable(L, 0, 2);
            lua_pushstring(L, "corrupt");
            lua_setfield(L, -2, "code");
            const std::string message = "namespace \"" + nameSpace + "\" of slot \"" + slot + "\" is not a dialogue state" +
                                        (error.empty() ? std::string() : ": " + error);
            lua_pushstring(L, message.c_str());
            lua_setfield(L, -2, "message");
            return 2;
        }
        lua_pushboolean(L, 1);
        lua_pushvalue(L, data + 1);
        return 2;
    }

    int luaVariablesResetVisits(lua_State*) {
        dialogue::Variables::resetVisits();
        return 0;
    }

    int luaVariablesIndex(lua_State* L) {
        const std::string_view field = luaL_checkstring(L, 2);
        if (field == "onChanged") {
            if (!luabridge::push(L, &dialogue::Variables::onChanged)) lua_pushnil(L);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Dialogue.variables.onChanged = function(name, value) ... end
    int luaVariablesNewIndex(lua_State* L) {
        const char* field = luaL_checkstring(L, 2);
        if (std::string_view(field) == "onChanged") {
            luaL_checktype(L, 3, LUA_TFUNCTION);
            lua_pushvalue(L, 3);
            dialogue::Variables::onChanged = L;
            lua_settop(L, 3);
            return 0;
        }
        return luaL_error(L, "Dialogue.variables.%s is read-only (use set)", field);
    }

    // {name = value}: start's message arguments (nil or a table, checked by the caller)
    LocalizationArgs argsAt(lua_State* L, int index) {
        LocalizationArgs args;
        if (lua_isnoneornil(L, index)) return args;
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

    // setArg's value: a number, a boolean or a string (luaL_checkstring'd by the caller otherwise)
    LocalizationValue argumentAt(lua_State* L, int index) {
        if (lua_isinteger(L, index)) return static_cast<int64_t>(lua_tointeger(L, index));
        if (lua_type(L, index) == LUA_TNUMBER) return static_cast<double>(lua_tonumber(L, index));
        if (lua_type(L, index) == LUA_TBOOLEAN) return std::string(lua_toboolean(L, index) ? "true" : "false");
        return std::string(lua_tostring(L, index));
    }

    DialogueRunner& runnerSelf(lua_State* L) {
        auto self = luabridge::get<DialogueRunner*>(L, 1);
        if (!self || !*self) luaL_argerror(L, 1, "a DialogueRunner (call it with ':')");
        return **self;
    }

#endif // JASPERENGINE_DIALOGUE_ENABLED

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

#if JASPERENGINE_DIALOGUE_ENABLED

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
#if JASPERENGINE_DIALOGUE_ENABLED
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
        {"save", luaVariablesSave},
        {"load", luaVariablesLoad},
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

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(DialogueRunner)>>("FunctionSubscribe_V_DialogueRunner")
        .addFunction("add", (bool (FunctionSubscribe<void(DialogueRunner)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(DialogueRunner)>::add)
        .addFunction("remove", &FunctionSubscribe<void(DialogueRunner)>::remove)
        .endClass()
        .beginClass<FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>>("FunctionSubscribe_V_DialogueLine")
        .addFunction("add", (bool (FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>::remove)
        .endClass()
        .beginClass<FunctionSubscribe<void(DialogueRunner, const std::string&)>>("FunctionSubscribe_V_DialogueEnd")
        .addFunction("add", (bool (FunctionSubscribe<void(DialogueRunner, const std::string&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(DialogueRunner, const std::string&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(DialogueRunner, const std::string&)>::remove)
        .endClass()
        .beginClass<FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>>("FunctionSubscribe_V_DialogueVariable")
        .addFunction("add", (bool (FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>::remove)
        .endClass()
        .beginClass<FunctionSubscribe<void(const std::string&, const dialogue::Value&)>>("FunctionSubscribe_V_DialogueChanged")
        .addFunction("add", (bool (FunctionSubscribe<void(const std::string&, const dialogue::Value&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::string&, const dialogue::Value&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::string&, const dialogue::Value&)>::remove)
        .endClass();

    // Plays a .dialogue on its GameObject (adds a Dialogue Runner when it has none); every line
    // reaches onLine, choose(i) picks among line.options, advance() goes on from a line without choices
    luabridge::getGlobalNamespace(L)
        .deriveClass<DialogueRunner, EntityHandle>("DialogueRunner")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("start", +[](lua_State* state) -> int {
            DialogueRunner& self = runnerSelf(state);
            const char* entry = luaL_optstring(state, 2, "");
            if (!lua_isnoneornil(state, 3)) luaL_checktype(state, 3, LUA_TTABLE);
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
            const char* name = luaL_checkstring(state, 2);
            const int type = lua_type(state, 3);
            if (type != LUA_TNUMBER && type != LUA_TBOOLEAN) luaL_checkstring(state, 3);
            self.setArg(name, argumentAt(state, 3));
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
