// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's gameplay classes (docs/specs/S22-gameplay.md R41): InputActions (actions, maps,
// bindings, virtual controls, injection, rebinding and its overrides) and its event type.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "input/InputActions.h"
#include "input/InputVirtualControls.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // a number (x) or a Vector2 at index
    Vector2 vectorArg(lua_State* L, int index){
        if (lua_type(L, index) == LUA_TNUMBER)
            return Vector2((float)lua_tonumber(L, index), 0.0f);
        auto value = luabridge::get<Vector2>(L, index);
        if (!value)
            luaL_argerror(L, index, "a number or a Vector2");
        return *value;
    }

    std::vector<std::string> stringList(lua_State* L, int index, const char* field){
        std::vector<std::string> list;
        lua_getfield(L, index, field);
        if (lua_istable(L, -1)){
            const lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= count; i++){
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING)
                    list.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }else if (lua_type(L, -1) == LUA_TSTRING){
            list.push_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        return list;
    }

    const char* rebindStatusName(InputRebindStatus status){
        switch (status){
            case InputRebindStatus::COMPLETED: return "completed";
            case InputRebindStatus::CANCELED: return "canceled";
            case InputRebindStatus::TIMED_OUT: return "timed_out";
        }
        return "canceled";
    }

    // InputActions.startRebind(action, bindingId, {groups, exclude, cancel, timeout}, function(result) end)
    int luaStartRebind(lua_State* L){
        const std::string action = luaL_checkstring(L, 1);
        const std::string bindingId = luaL_checkstring(L, 2);
        InputRebindOptions options;
        if (lua_istable(L, 3)){
            options.groups = stringList(L, 3, "groups");
            options.exclude = stringList(L, 3, "exclude");
            lua_getfield(L, 3, "cancel");
            if (lua_type(L, -1) == LUA_TSTRING) options.cancel = lua_tostring(L, -1);
            else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1)) options.cancel.clear();
            lua_pop(L, 1);
            lua_getfield(L, 3, "timeout");
            if (lua_type(L, -1) == LUA_TNUMBER) options.timeout = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        std::function<void(const InputRebindResult&)> callback;
        if (lua_isfunction(L, 4)){
            luabridge::LuaRef function = luabridge::LuaRef::fromStack(L, 4);
            callback = [function](const InputRebindResult& result){
                lua_State* state = function.state();
                luabridge::LuaRef table = luabridge::newTable(state);
                table["status"] = rebindStatusName(result.status);
                table["action"] = result.action;
                table["bindingId"] = result.bindingId;
                table["path"] = result.path;
                auto called = function(table);
                if (!called)
                    Log::error("InputActions.startRebind callback: %s", called.errorMessage().c_str());
            };
        }
        lua_pushboolean(L, InputActions::startRebind(action, bindingId, options, std::move(callback)));
        return 1;
    }

    // a list of {action, id, path, groups}
    int luaGetConflicts(lua_State* L){
        const std::vector<InputBindingInfo> conflicts = InputActions::getConflicts(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
        lua_createtable(L, (int)conflicts.size(), 0);
        for (size_t i = 0; i < conflicts.size(); i++){
            lua_createtable(L, 0, 4);
            lua_pushstring(L, conflicts[i].action.c_str());
            lua_setfield(L, -2, "action");
            lua_pushstring(L, conflicts[i].id.c_str());
            lua_setfield(L, -2, "id");
            lua_pushstring(L, conflicts[i].path.c_str());
            lua_setfield(L, -2, "path");
            lua_createtable(L, (int)conflicts[i].groups.size(), 0);
            for (size_t g = 0; g < conflicts[i].groups.size(); g++){
                lua_pushstring(L, conflicts[i].groups[g].c_str());
                lua_rawseti(L, -2, (lua_Integer)g + 1);
            }
            lua_setfield(L, -2, "groups");
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        return 1;
    }

    int luaSetVirtual(lua_State* L){
        InputActions::setVirtual(luaL_checkstring(L, 1), vectorArg(L, 2));
        return 0;
    }

    int luaInject(lua_State* L){
        InputActions::inject(luaL_checkstring(L, 1), vectorArg(L, 2), (int)luaL_optinteger(L, 3, 1));
        return 0;
    }

}

void LuaBinding::registerGameplayClasses(lua_State *L){

    luabridge::getGlobalNamespace(L)
        .beginNamespace("InputActionPhase")
        .addVariable("WAITING", InputActionPhase::WAITING)
        .addVariable("STARTED", InputActionPhase::STARTED)
        .addVariable("PERFORMED", InputActionPhase::PERFORMED)
        .addVariable("CANCELED", InputActionPhase::CANCELED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("InputActionType")
        .addVariable("BUTTON", InputActionType::BUTTON)
        .addVariable("AXIS", InputActionType::AXIS)
        .addVariable("VECTOR2", InputActionType::VECTOR2)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputActionEvent>("InputActionEvent")
        .addProperty("action", &InputActionEvent::getAction)
        .addProperty("phase", &InputActionEvent::getPhase)
        .addProperty("value", &InputActionEvent::getValue)
        .addProperty("vector2", &InputActionEvent::getVector2)
        .addProperty("consumed", &InputActionEvent::isConsumed)
        .addFunction("consume", &InputActionEvent::consume)
        .endClass();

    // (not "FunctionSubscribe_V_IAE": that is upstream's (int, AnimationEvent) of the Vertex
    // Animator's instance events; one Lua name for two types asserts in LuaBridge's debug builds)
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(InputActionEvent&)>>("FunctionSubscribe_V_InputActionEvent")
        .addFunction("__call", &FunctionSubscribe<void(InputActionEvent&)>::call)
        .addFunction("call", &FunctionSubscribe<void(InputActionEvent&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(InputActionEvent&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(InputActionEvent&)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputBinding>("InputBinding")
        .addProperty("id", [] (const InputBinding* self) { return self->getId(); })
        .addProperty("path", &InputBinding::getPath)
        .addProperty("displayName", &InputBinding::getDisplayName)
        .addProperty("valid", &InputBinding::isValid)
        .addProperty("composite", &InputBinding::isComposite)
        .addProperty("overridden", &InputBinding::hasOverride)
        .addFunction("getDisplayName", &InputBinding::getDisplayName)
        .addFunction("getGroups", &InputBinding::getGroups)
        .addFunction("getParts", &InputBinding::getParts)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputAction>("InputAction")
        .addProperty("name", [] (const InputAction* self) { return self->getName(); })
        .addProperty("mapName", [] (const InputAction* self) { return self->getMapName(); })
        .addProperty("path", &InputAction::getPath)
        .addProperty("type", &InputAction::getType)
        .addProperty("phase", &InputAction::getPhase)
        .addProperty("consumedBy", &InputAction::getConsumedBy)
        .addFunction("readValue", &InputAction::readValue)
        .addFunction("readVector2", &InputAction::readVector2)
        .addFunction("isPressed", &InputAction::isPressed)
        .addFunction("wasPressedThisFrame", &InputAction::wasPressedThisFrame)
        .addFunction("wasReleasedThisFrame", &InputAction::wasReleasedThisFrame)
        .addFunction("getHeldTime", &InputAction::getHeldTime)
        .addFunction("getPhase", &InputAction::getPhase)
        .addFunction("getBindings", &InputAction::getBindings)
        .addProperty("onStarted", [] (InputAction* self) { return &self->onStarted; }, [] (InputAction* self, lua_State* L) { self->onStarted = L; })
        .addProperty("onPerformed", [] (InputAction* self) { return &self->onPerformed; }, [] (InputAction* self, lua_State* L) { self->onPerformed = L; })
        .addProperty("onCanceled", [] (InputAction* self) { return &self->onCanceled; }, [] (InputAction* self, lua_State* L) { self->onCanceled = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputActionMap>("InputActionMap")
        .addProperty("name", [] (const InputActionMap* self) { return self->getName(); })
        .addProperty("valid", &InputActionMap::isValid)
        .addProperty("enabled", &InputActionMap::isEnabled, &InputActionMap::setEnabled)
        .addProperty("priority", &InputActionMap::getPriority, &InputActionMap::setPriority)
        .addProperty("gamepad", &InputActionMap::getGamepad, &InputActionMap::setGamepad)
        .addFunction("enable", &InputActionMap::enable)
        .addFunction("disable", &InputActionMap::disable)
        .addFunction("setGamepad", &InputActionMap::setGamepad)
        .addFunction("getActions", &InputActionMap::getActions)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputActions>("InputActions")
        .addStaticProperty("defaultDeadzone", &InputActions::getDefaultDeadzone, &InputActions::setDefaultDeadzone)
        .addStaticFunction("load", &InputActions::load)
        .addStaticFunction("loadDefault", &InputActions::loadDefault)
        .addStaticFunction("unload", &InputActions::unload)
        .addStaticFunction("isLoaded", &InputActions::isLoaded)
        .addStaticFunction("getAssetPath", [] () { return InputActions::getAssetPath(); })
        .addStaticFunction("get", &InputActions::get)
        .addStaticFunction("getMap", &InputActions::getMap)
        .addStaticFunction("getBinding", &InputActions::getBinding)
        .addStaticFunction("getActions", &InputActions::getActions)
        .addStaticFunction("getMapNames", &InputActions::getMapNames)
        .addStaticFunction("enableMap", &InputActions::enableMap)
        .addStaticFunction("isMapEnabled", &InputActions::isMapEnabled)
        .addStaticFunction("setMapPriority", &InputActions::setMapPriority)
        .addStaticFunction("getMapPriority", &InputActions::getMapPriority)
        .addStaticFunction("setGamepad", &InputActions::setGamepad)
        .addStaticFunction("getGamepad", &InputActions::getGamepad)
        .addStaticFunction("setVirtual", &luaSetVirtual)
        .addStaticFunction("getVirtual", &InputActions::getVirtual)
        .addStaticFunction("inject", &luaInject)
        .addStaticFunction("releaseInjected", &InputActions::releaseInjected)
        .addStaticFunction("startRebind", &luaStartRebind)
        .addStaticFunction("cancelRebind", &InputActions::cancelRebind)
        .addStaticFunction("isRebinding", &InputActions::isRebinding)
        .addStaticFunction("getConflicts", &luaGetConflicts)
        .addStaticFunction("getBindingPath", &InputActions::getBindingPath)
        .addStaticFunction("setOverride", &InputActions::setOverride)
        .addStaticFunction("removeOverride", &InputActions::removeOverride)
        .addStaticFunction("saveOverrides", &InputActions::saveOverrides)
        .addStaticFunction("loadOverrides", &InputActions::loadOverrides)
        .addStaticFunction("resetOverrides", &InputActions::resetOverrides)
        .addStaticFunction("getBindingDisplayName", &InputActions::getBindingDisplayName)
        .addStaticFunction("getControlDisplayName", [] (const std::string& path) { return InputActions::getControlDisplayName(path, 0); })
        .addStaticFunction("isControlBound", &InputActions::isControlBound)
        // on-screen controls: HTML UI elements as virtual/<name> sticks and buttons
        .addStaticFunction("bindVirtualStick", &InputVirtualControls::bindStick)
        .addStaticFunction("bindVirtualButton", &InputVirtualControls::bindButton)
        .addStaticFunction("unbindVirtualControl", &InputVirtualControls::unbind)
        .endClass();
}
