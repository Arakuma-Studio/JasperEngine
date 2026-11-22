// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptEvent.h"
#include "LuaBinding.h"
#include "Log.h"
#include "util/CancellableEvent.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <cctype>

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

    // RegisterEvent's tag `<Class>_<address>_<method>` as "Class:method"; another tag as itself
    std::string registeredListenerName(const std::string& tag) {
        // the address: the first run of 6 or more digits between underscores
        for (size_t start = tag.find('_'); start != std::string::npos; start = tag.find('_', start + 1)) {
            size_t end = start + 1;
            while (end < tag.size() && std::isdigit(static_cast<unsigned char>(tag[end]))) {
                end++;
            }
            if (end - start - 1 >= 6 && end < tag.size() && tag[end] == '_' && start > 0 && end + 1 < tag.size()) {
                return tag.substr(0, start) + ":" + tag.substr(end + 1);
            }
        }
        return tag;
    }
}

ScriptEvent::ScriptEvent(): ScriptEvent(false) {
}

ScriptEvent::ScriptEvent(bool cancellable): cancellable(cancellable) {
    state = LuaBinding::getLuaState();
    std::fill(std::begin(cachedArgs), std::end(cachedArgs), LUA_NOREF);
    std::fill(std::begin(cachedArgsData), std::end(cachedArgsData), nullptr);
}

ScriptEvent::~ScriptEvent() {
    // the state is the one the Event lives in (garbage collected, or closed with it)
    for (const auto& listener : listeners) {
        release(*listener);
    }
    for (const auto& listener : pending) {
        release(*listener);
    }
    listeners.clear();
    pending.clear();
    if (state) {
        for (int& ref : cachedArgs) {
            if (ref != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX, ref);
                ref = LUA_NOREF;
            }
        }
    }
}

CancellableScriptEvent::CancellableScriptEvent(): ScriptEvent(true) {
}

std::optional<std::string> CancellableScriptEvent::cancelledBy() const {
    if (!lastCancelled) {
        return std::nullopt;
    }
    return lastCancelledBy;
}

int32_t ScriptEvent::checkPriority(lua_State* L, int index, const char* caller) {
    if (lua_isnoneornil(L, index)) {
        return 0;
    }
    int isInteger = 0;
    const lua_Integer value = lua_type(L, index) == LUA_TNUMBER ? lua_tointegerx(L, index, &isInteger) : 0;
    if (isInteger && (value == EventPriority::Monitor || (value >= EventPriority::Min && value <= EventPriority::Max))) {
        return static_cast<int32_t>(value);
    }
    luaL_error(L, "%s: priority must be an integer from -1000000 to 1000000 or Event.MONITOR", caller);
    return 0;
}

int ScriptEvent::monitorPriority() {
    return EventPriority::Monitor;
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
    // a running invoke walks `listeners` by index: it compacts once the outermost one ends
    if (depth > 0) {
        dirty = true;
        return;
    }
    auto removed = [](const std::shared_ptr<Listener>& listener) { return listener->removed; };
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(), removed), listeners.end());
    for (auto& listener : pending) {
        if (!listener->removed) {
            insertSorted(listeners, std::move(listener));
        }
    }
    pending.clear();
    dirty = false;
}

void ScriptEvent::insert(std::shared_ptr<Listener> listener) {
    if (depth > 0) {
        pending.push_back(std::move(listener));
    } else {
        insertSorted(listeners, std::move(listener));
    }
}

void ScriptEvent::insertSorted(std::vector<std::shared_ptr<Listener>>& list, std::shared_ptr<Listener> listener) {
    // after the last listener of a priority >= its own: an append while every priority is 0
    if (list.empty() || list.back()->priority >= listener->priority) {
        list.push_back(std::move(listener));
        return;
    }
    auto at = std::upper_bound(list.begin(), list.end(), listener->priority,
        [](int32_t priority, const std::shared_ptr<Listener>& other) { return priority > other->priority; });
    list.insert(at, std::move(listener));
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
    for (const auto* list : {&listeners, &pending}) {
        for (const auto& listener : *list) {
            if (!listener->removed && ownerReleased(state, *listener)) {
                release(*listener);
            }
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
    for (const auto* list : {&listeners, &pending}) {
        for (const auto& listener : *list) {
            if (!listener->removed && sameRef(L, listener->owner, ownerIndex)) {
                release(*listener);
            }
        }
    }
    compact();
}

void ScriptEvent::addTagged(lua_State* L, int eventIndex, const std::string& tag, int functionIndex, int ownerIndex, int32_t priority) {
    functionIndex = lua_absindex(L, functionIndex);
    eventIndex = lua_absindex(L, eventIndex);
    if (ownerIndex != 0) {
        ownerIndex = lua_absindex(L, ownerIndex);
    }
    removeTag(tag);
    auto listener = std::make_shared<Listener>();
    listener->tag = tag;
    listener->priority = priority;
    lua_pushvalue(L, functionIndex);
    listener->function = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ownerIndex != 0 && lua_istable(L, ownerIndex)) {
        lua_pushvalue(L, ownerIndex);
        listener->owner = luaL_ref(L, LUA_REGISTRYINDEX);
        trackOwner(L, ownerIndex, eventIndex);
    }
    insert(std::move(listener));
}

bool ScriptEvent::removeTag(const std::string& tag) {
    for (const auto* list : {&listeners, &pending}) {
        for (const auto& listener : *list) {
            if (!listener->removed && !listener->tag.empty() && listener->tag == tag) {
                release(*listener);
                compact();
                return true;
            }
        }
    }
    return false;
}

bool ScriptEvent::hasTag(const std::string& tag) const {
    for (const auto* list : {&listeners, &pending}) {
        for (const auto& listener : *list) {
            if (!listener->removed && !listener->tag.empty() && listener->tag == tag && !ownerReleased(state, *listener)) {
                return true;
            }
        }
    }
    return false;
}

int ScriptEvent::listenerCount() const {
    dropReleasedOwners();
    int count = 0;
    for (const auto* list : {&listeners, &pending}) {
        for (const auto& listener : *list) {
            count += listener->removed ? 0 : 1;
        }
    }
    return count;
}

int ScriptEvent::luaAddListener(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const char* caller = self->cancellable ? "CancellableEvent:addListener" : "Event:addListener";
    if (lua_isfunction(L, 2)) {
        const int32_t priority = checkPriority(L, 3, caller);
        for (const auto* list : {&self->listeners, &self->pending}) {
            for (const auto& listener : *list) {
                if (!listener->removed && listener->tag.empty() && sameRef(L, listener->function, 2)) {
                    return 0; // added already, whatever its priority
                }
            }
        }
        auto listener = std::make_shared<Listener>();
        lua_pushvalue(L, 2);
        listener->function = luaL_ref(L, LUA_REGISTRYINDEX);
        listener->priority = priority;
        self->insert(std::move(listener));
        return 0;
    }
    if (lua_istable(L, 2) && lua_type(L, 3) == LUA_TSTRING) {
        const int32_t priority = checkPriority(L, 4, caller);
        // checked before any C++ object lives in this frame: Lua is compiled as C, and luaL_error
        // longjmps over the frame (no destructor runs; MSVC may end the process)
        const char* methodName = lua_tostring(L, 3);
        lua_getfield(L, 2, methodName);
        const bool callable = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (!callable) {
            return luaL_error(L, "%s: the script has no method '%s'", caller, methodName);
        }
        const std::string method = methodName;
        for (const auto* list : {&self->listeners, &self->pending}) {
            for (const auto& listener : *list) {
                if (!listener->removed && listener->function == LUA_NOREF && listener->method == method && sameRef(L, listener->owner, 2)) {
                    return 0;
                }
            }
        }
        auto listener = std::make_shared<Listener>();
        lua_pushvalue(L, 2);
        listener->owner = luaL_ref(L, LUA_REGISTRYINDEX);
        listener->method = method;
        listener->priority = priority;
        self->insert(std::move(listener));
        trackOwner(L, 2, 1);
        return 0;
    }
    return luaL_error(L, "%s(fn [, priority]) or %s(self, \"methodName\" [, priority])", caller, caller);
}

int ScriptEvent::luaRemoveListener(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const bool byFunction = lua_isfunction(L, 2);
    const bool byMethod = lua_istable(L, 2) && lua_type(L, 3) == LUA_TSTRING;
    if (!byFunction && !byMethod) {
        return luaL_error(L, "Event:removeListener(fn) or Event:removeListener(self, \"methodName\")");
    }
    const std::string method = byMethod ? lua_tostring(L, 3) : "";
    for (const auto* list : {&self->listeners, &self->pending}) {
        for (const auto& listener : *list) {
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
    }
    lua_pushboolean(L, 0);
    return 1;
}

int ScriptEvent::luaRemoveAllListeners(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    for (const auto* list : {&self->listeners, &self->pending}) {
        for (const auto& listener : *list) {
            self->release(*listener);
        }
    }
    self->compact();
    return 0;
}

ScriptCancelEventArgs* ScriptEvent::pushCancelArgs(lua_State* L) {
    // one per nesting depth, made on first use: an invoke allocates nothing
    ScriptCancelEventArgs* args = nullptr;
    if (depth < CACHED_ARGS && cachedArgs[depth] != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cachedArgs[depth]);
        args = cachedArgsData[depth];
    } else {
        if (!luabridge::push(L, ScriptCancelEventArgs())) {
            luaL_error(L, "CancellableEvent:invoke: CancelEventArgs is not registered");
        }
        args = luabridge::detail::Userdata::get<ScriptCancelEventArgs>(L, -1, false);
        if (depth < CACHED_ARGS) {
            lua_pushvalue(L, -1);
            cachedArgs[depth] = luaL_ref(L, LUA_REGISTRYINDEX);
            cachedArgsData[depth] = args;
        }
    }
    args->cancelled = false;
    args->monitor = false;
    args->ended = false;
    args->event = this;
    args->current = nullptr;
    args->cancelledBy.clear();
    return args;
}

int ScriptEvent::luaInvoke(lua_State* L) {
    return dispatch(L, eventArg(L));
}

int CancellableScriptEvent::luaInvoke(lua_State* L) {
    ScriptEvent* self = luabridge::detail::Userdata::get<CancellableScriptEvent>(L, 1, false);
    if (!self) {
        luaL_error(L, "CancellableEvent:invoke called without a CancellableEvent (use a colon: ev:invoke())");
    }
    return dispatch(L, self);
}

int ScriptEvent::dispatch(lua_State* L, ScriptEvent* self) {
    if (self->listeners.empty()) {
        if (!self->cancellable) {
            return 0;
        }
        self->invokedOnce = true;
        self->lastCancelled = false;
        lua_pushboolean(L, 0);
        return 1;
    }
    const int nargs = lua_gettop(L) - 1;
    // raised here, before the dispatch state changes (an error in the loop would skip its end)
    luaL_checkstack(L, nargs + LUA_MINSTACK, "Event:invoke: too many arguments");
    ScriptCancelEventArgs* args = nullptr;
    int argsIndex = 0;
    if (self->cancellable) {
        args = self->pushCancelArgs(L);
        argsIndex = lua_gettop(L);
    }
    // the listeners as they are now: one added meanwhile waits in `pending` for the next invoke,
    // one removed meanwhile is skipped at once (its removed flag); nothing is copied
    self->depth++;
    const size_t count = self->listeners.size();
    for (size_t i = 0; i < count; i++) {
        Listener& listener = *self->listeners[i];
        if (listener.removed) {
            continue;
        }
        if (self->ownerReleased(L, listener)) {
            self->release(listener);
            self->dirty = true;
            continue;
        }
        if (args) {
            if (listener.priority == EventPriority::Monitor) {
                args->monitor = true;   // monitors sort last
            } else if (args->cancelled) {
                continue;               // a deciding listener after the veto
            }
            args->current = &listener;
        }
        const int top = lua_gettop(L);
        int callArgs = nargs;
        if (listener.function != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, listener.function);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, listener.owner);
            LuaBinding::protectedGetField(L, -1, listener.method.c_str());
            if (!lua_isfunction(L, -1)) {
                Log::error("Event listener: the script has no method '%s' any more", listener.method.c_str());
                lua_settop(L, top);
                continue;
            }
            lua_insert(L, -2); // method, owner
            callArgs++;
        }
        if (args) {
            lua_pushvalue(L, argsIndex);
            callArgs++;
        }
        for (int a = 2; a <= nargs + 1; a++) {
            lua_pushvalue(L, a);
        }
        if (LuaBinding::pcallWithTraceback(L, callArgs, 0) != LUA_OK) {
            const std::string where = listener.method.empty() ? std::string() : " (" + listener.method + ")";
            Log::error("Lua Event listener%s failed: %s", where.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
        }
        lua_settop(L, top);
    }
    self->depth--;
    if (self->dirty || !self->pending.empty()) {
        self->compact();
    }
    if (!args) {
        return 0;
    }
    args->ended = true;
    args->current = nullptr;
    self->invokedOnce = true;
    self->lastCancelled = args->cancelled;
    if (args->cancelled) {
        self->lastCancelledBy = std::move(args->cancelledBy);
    }
    lua_pushboolean(L, self->lastCancelled ? 1 : 0);
    return 1;
}

int ScriptEvent::luaAdd(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    const char* caller = self->cancellable ? "CancellableEvent:add" : "Event:add";
    if (lua_type(L, 2) != LUA_TSTRING || !lua_isfunction(L, 3)) {
        return luaL_error(L, "%s(tag, fn [, priority])", caller);
    }
    const int32_t priority = checkPriority(L, 4, caller);
    self->addTagged(L, 1, lua_tostring(L, 2), 3, 0, priority);
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

std::string ScriptEvent::listenerName(lua_State* L, const Listener& listener) const {
    if (!listener.method.empty()) {
        std::string className = "Script";
        if (listener.owner != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, listener.owner);
            LuaBinding::protectedGetField(L, -1, "__name");
            if (lua_type(L, -1) == LUA_TSTRING) {
                className = lua_tostring(L, -1);
            }
            lua_pop(L, 2);
        }
        return className + ":" + listener.method;
    }
    if (!listener.tag.empty()) {
        return registeredListenerName(listener.tag);
    }
    if (listener.function == LUA_NOREF) {
        return "function (removed)";
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, listener.function);
    lua_Debug ar;
    lua_getinfo(L, ">S", &ar);
    return std::string("function ") + ar.short_src + ":" + std::to_string(ar.linedefined);
}

int ScriptEvent::luaGetListeners(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    self->dropReleasedOwners();
    // during an invoke the listeners added meanwhile are where they will run from the next one
    std::vector<const Listener*> order;
    for (const auto& listener : self->listeners) {
        if (!listener->removed) {
            order.push_back(listener.get());
        }
    }
    for (const auto& listener : self->pending) {
        if (listener->removed) {
            continue;
        }
        auto at = std::upper_bound(order.begin(), order.end(), listener->priority,
            [](int32_t priority, const Listener* other) { return priority > other->priority; });
        order.insert(at, listener.get());
    }
    lua_createtable(L, static_cast<int>(order.size()), 0);
    for (size_t i = 0; i < order.size(); i++) {
        lua_createtable(L, 0, 3);
        const std::string name = self->listenerName(L, *order[i]);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, order[i]->priority);
        lua_setfield(L, -2, "priority");
        lua_pushboolean(L, order[i]->priority == EventPriority::Monitor ? 1 : 0);
        lua_setfield(L, -2, "monitor");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int ScriptEvent::luaCancel(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    if (self->cancellable) {
        return luaL_error(L, "CancellableEvent:cancel: cancel through the listener's first argument (e:cancel())");
    }
    return luaL_error(L, "Event:cancel: this Event cannot be cancelled; make it with CancellableEvent() and cancel "
                         "through the listener's first argument");
}

int ScriptEvent::luaToString(lua_State* L) {
    ScriptEvent* self = eventArg(L);
    if (!self->cancellable) {
        lua_pushfstring(L, "Event(%d listeners)", self->listenerCount());
    } else if (!self->invokedOnce) {
        lua_pushfstring(L, "CancellableEvent(%d listeners, not invoked yet)", self->listenerCount());
    } else if (self->lastCancelled) {
        lua_pushfstring(L, "CancellableEvent(%d listeners, last invoke cancelled by %s)", self->listenerCount(),
                        self->lastCancelledBy.c_str());
    } else {
        lua_pushfstring(L, "CancellableEvent(%d listeners, last invoke went ahead)", self->listenerCount());
    }
    return 1;
}

int ScriptCancelEventArgs::luaCancel(lua_State* L) {
    ScriptCancelEventArgs* self = luabridge::detail::Userdata::get<ScriptCancelEventArgs>(L, 1, false);
    if (!self) {
        return luaL_error(L, "CancelEventArgs:cancel called without its CancelEventArgs (use a colon: e:cancel())");
    }
    if (self->ended) {
        return luaL_error(L, "CancelEventArgs:cancel: the invoke it belongs to has ended; cancel inside the listener");
    }
    if (self->monitor) {
        return luaL_error(L, "CancelEventArgs:cancel: a monitor listener runs after the decision and cannot cancel; "
                             "give it a priority instead");
    }
    if (!self->cancelled) {
        self->cancelled = true;
        if (self->current) {
            self->cancelledBy = self->event->listenerName(L, *self->current);
        }
    }
    return 0;
}
