// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptEvent.h"
#include "LuaBinding.h"
#include "Log.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>

using namespace jasperengine;

namespace {
    // registry: owner instance -> {Event userdata = true}, weak keys in both
    const char* const EVENT_OWNERS = "jasperengine.eventOwners";

    void pushWeakTable(lua_State* L) {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
    }

    ScriptEvent* eventArg(lua_State* L) {
        ScriptEvent* event = luabridge::detail::Userdata::get<ScriptEvent>(L, 1, false);
        if (!event) {
            luaL_error(L, "Event method called without an Event (use a colon: ev:invoke())");
        }
        return event;
    }

    bool sameRef(lua_State* L, int ref, int index) {
        if (ref == LUA_NOREF) {
            return false;
        }
        index = lua_absindex(L, index);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const bool same = lua_rawequal(L, -1, index) != 0;
        lua_pop(L, 1);
        return same;
    }
}

ScriptEvent::ScriptEvent() {
    state = LuaBinding::getLuaState();
}

ScriptEvent::~ScriptEvent() {
    // the state is the one the Event lives in (garbage collected, or closed with it)
    for (const auto& listener : listeners) {
        release(*listener);
    }
    listeners.clear();
}

void ScriptEvent::release(Listener& listener) const {
    listener.removed = true;
    if (!state) {
        return;
    }
    if (listener.function != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, listener.function);
        listener.function = LUA_NOREF;
    }
    if (listener.owner != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, listener.owner);
        listener.owner = LUA_NOREF;
    }
}

void ScriptEvent::compact() const {
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
        [](const std::shared_ptr<Listener>& listener) { return listener->removed; }), listeners.end());
}

bool ScriptEvent::ownerReleased(lua_State* L, const Listener& listener) const {
    if (listener.owner == LUA_NOREF || !L) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, listener.owner);
    lua_pushstring(L, "__released");
    lua_rawget(L, -2);
    const bool released = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return released;
}

void ScriptEvent::dropReleasedOwners() const {
    for (const auto& listener : listeners) {
        if (!listener->removed && ownerReleased(state, *listener)) {
            release(*listener);
        }
    }
    compact();
}

void ScriptEvent::trackOwner(lua_State* L, int ownerIndex, int eventIndex) {
    ownerIndex = lua_absindex(L, ownerIndex);
    eventIndex = lua_absindex(L, eventIndex);
    if (lua_getfield(L, LUA_REGISTRYINDEX, EVENT_OWNERS) != LUA_TTABLE) {
        lua_pop(L, 1);
        pushWeakTable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, EVENT_OWNERS);
    }
    lua_pushvalue(L, ownerIndex);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        pushWeakTable(L);
        lua_pushvalue(L, ownerIndex);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_pushvalue(L, eventIndex);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 2);
}

void ScriptEvent::releaseOwner(lua_State* L, int ownerIndex) {
    ownerIndex = lua_absindex(L, ownerIndex);
    if (lua_getfield(L, LUA_REGISTRYINDEX, EVENT_OWNERS) != LUA_TTABLE) {
        lua_pop(L, 1);
        return;
    }
    const int owners = lua_gettop(L);
    lua_pushvalue(L, ownerIndex);
    lua_rawget(L, owners);
    if (lua_istable(L, -1)) {
        const int events = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, events) != 0) {
            lua_pop(L, 1);
            if (ScriptEvent* event = luabridge::detail::Userdata::get<ScriptEvent>(L, -1, false)) {
                event->dropOwner(L, ownerIndex);
            }
        }
    }
    lua_pop(L, 1);
    lua_pushvalue(L, ownerIndex);
    lua_pushnil(L);
    lua_rawset(L, owners);
    lua_pop(L, 1);
}

void ScriptEvent::dropOwner(lua_State* L, int ownerIndex) {
    for (const auto& listener : listeners) {
        if (!listener->removed && sameRef(L, listener->owner, ownerIndex)) {
            release(*listener);
        }
    }
    compact();
}

void ScriptEvent::addTagged(lua_State* L, int eventIndex, const std::string& tag, int functionIndex, int ownerIndex) {
    functionIndex = lua_absindex(L, functionIndex);
    eventIndex = lua_absindex(L, eventIndex);
    if (ownerIndex != 0) {
        ownerIndex = lua_absindex(L, ownerIndex);
    }
    removeTag(tag);
    auto listener = std::make_shared<Listener>();
    listener->tag = tag;
    lua_pushvalue(L, functionIndex);
    listener->function = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ownerIndex != 0 && lua_istable(L, ownerIndex)) {
        lua_pushvalue(L, ownerIndex);
        listener->owner = luaL_ref(L, LUA_REGISTRYINDEX);
        trackOwner(L, ownerIndex, eventIndex);
    }
    listeners.push_back(listener);
}

bool ScriptEvent::removeTag(const std::string& tag) {
    for (const auto& listener : listeners) {
        if (!listener->removed && !listener->tag.empty() && listener->tag == tag) {
            release(*listener);
            compact();
            return true;
        }
    }
    return false;
}

bool ScriptEvent::hasTag(const std::string& tag) const {
    for (const auto& listener : listeners) {
        if (!listener->removed && !listener->tag.empty() && listener->tag == tag && !ownerReleased(state, *listener)) {
            return true;
        }
    }
    return false;
}

int ScriptEvent::listenerCount() const {
    dropReleasedOwners();
    return static_cast<int>(listeners.size());
}

int ScriptEvent::luaAddListener(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    if (lua_isfunction(L, 2)) {
        for (const auto& listener : self->listeners) {
            if (!listener->removed && listener->tag.empty() && sameRef(L, listener->function, 2)) {
                return 0; // added already
            }
        }
        auto listener = std::make_shared<Listener>();
        lua_pushvalue(L, 2);
        listener->function = luaL_ref(L, LUA_REGISTRYINDEX);
        self->listeners.push_back(listener);
        return 0;
    }
    if (lua_istable(L, 2) && lua_type(L, 3) == LUA_TSTRING) {
        const std::string method = lua_tostring(L, 3);
        lua_getfield(L, 2, method.c_str());
        const bool callable = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (!callable) {
            return luaL_error(L, "Event:addListener: the script has no method '%s'", method.c_str());
        }
        for (const auto& listener : self->listeners) {
            if (!listener->removed && listener->function == LUA_NOREF && listener->method == method && sameRef(L, listener->owner, 2)) {
                return 0;
            }
        }
        auto listener = std::make_shared<Listener>();
        lua_pushvalue(L, 2);
        listener->owner = luaL_ref(L, LUA_REGISTRYINDEX);
        listener->method = method;
        self->listeners.push_back(listener);
        trackOwner(L, 2, 1);
        return 0;
    }
    return luaL_error(L, "Event:addListener(fn) or Event:addListener(self, \"methodName\")");
}

int ScriptEvent::luaRemoveListener(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const bool byFunction = lua_isfunction(L, 2);
    const bool byMethod = lua_istable(L, 2) && lua_type(L, 3) == LUA_TSTRING;
    if (!byFunction && !byMethod) {
        return luaL_error(L, "Event:removeListener(fn) or Event:removeListener(self, \"methodName\")");
    }
    const std::string method = byMethod ? lua_tostring(L, 3) : "";
    for (const auto& listener : self->listeners) {
        if (listener->removed) {
            continue;
        }
        const bool match = byFunction ? sameRef(L, listener->function, 2)
                                      : (listener->function == LUA_NOREF && listener->method == method && sameRef(L, listener->owner, 2));
        if (match) {
            self->release(*listener);
            self->compact();
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int ScriptEvent::luaRemoveAllListeners(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    for (const auto& listener : self->listeners) {
        self->release(*listener);
    }
    self->listeners.clear();
    return 0;
}

int ScriptEvent::luaInvoke(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const int nargs = lua_gettop(L) - 1;
    // the listeners as they are now: one added meanwhile runs from the next invoke, one removed
    // meanwhile is skipped at once (its removed flag)
    const std::vector<std::shared_ptr<Listener>> snapshot = self->listeners;
    for (const auto& listener : snapshot) {
        if (listener->removed) {
            continue;
        }
        if (self->ownerReleased(L, *listener)) {
            self->release(*listener);
            continue;
        }
        const int top = lua_gettop(L);
        int callArgs = nargs;
        if (listener->function != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, listener->function);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, listener->owner);
            LuaBinding::protectedGetField(L, -1, listener->method.c_str());
            if (!lua_isfunction(L, -1)) {
                Log::error("Event listener: the script has no method '%s' any more", listener->method.c_str());
                lua_settop(L, top);
                continue;
            }
            lua_insert(L, -2); // method, owner
            callArgs++;
        }
        for (int i = 2; i <= nargs + 1; i++) {
            lua_pushvalue(L, i);
        }
        if (LuaBinding::pcallWithTraceback(L, callArgs, 0) != LUA_OK) {
            const std::string where = listener->method.empty() ? std::string() : " (" + listener->method + ")";
            Log::error("Lua Event listener%s failed: %s", where.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
        }
        lua_settop(L, top);
    }
    self->compact();
    return 0;
}

int ScriptEvent::luaAdd(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    if (lua_type(L, 2) != LUA_TSTRING || !lua_isfunction(L, 3)) {
        return luaL_error(L, "Event:add(tag, fn)");
    }
    self->addTagged(L, 1, lua_tostring(L, 2), 3, 0);
    lua_pushboolean(L, 1);
    return 1;
}

int ScriptEvent::luaRemove(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const char* tag = luaL_checkstring(L, 2);
    lua_pushboolean(L, self->removeTag(tag) ? 1 : 0);
    return 1;
}

int ScriptEvent::luaHas(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const char* tag = luaL_checkstring(L, 2);
    lua_pushboolean(L, self->hasTag(tag) ? 1 : 0);
    return 1;
}

int ScriptEvent::luaToString(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    lua_pushfstring(L, "Event(%d listeners)", self->listenerCount());
    return 1;
}
