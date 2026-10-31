// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_EVENT_H
#define SCRIPT_EVENT_H

#include "Export.h"

#include <memory>
#include <string>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    // Lua's `Event()` (Unity's UnityEvent / C# event): listeners called in the order they were
    // added. A listener is a function, or a method of a script instance that owns it
    // (`ev:addListener(self, "onScore")`), dropped once that instance is released (its
    // GameObject destroyed or its scene unloaded). Lua only, main thread; the listeners are
    // registry references released with the Event.
    class JASPERENGINE_API ScriptEvent {
    public:
        ScriptEvent();
        ~ScriptEvent();
        ScriptEvent(const ScriptEvent&) = delete;
        ScriptEvent& operator=(const ScriptEvent&) = delete;

        // RegisterEvent(self, ev, "method" [, tag]): a function under a tag (one per tag),
        // owned by the script at ownerIndex (0: none); eventIndex is this Event's userdata
        void addTagged(lua_State* L, int eventIndex, const std::string& tag, int functionIndex, int ownerIndex);
        // The script instance at `ownerIndex` is released: every Event drops the listeners it
        // owns (their registry references would keep it alive)
        static void releaseOwner(lua_State* L, int ownerIndex);
        bool removeTag(const std::string& tag);
        bool hasTag(const std::string& tag) const;
        // the listeners (those of released owners are dropped first)
        int listenerCount() const;

        // Lua methods (argument 1 is the Event)
        static int luaAddListener(lua_State* L);      // (fn) or (owner, "method")
        static int luaRemoveListener(lua_State* L);   // same arguments -> bool
        static int luaRemoveAllListeners(lua_State* L);
        static int luaInvoke(lua_State* L);           // (...) the listeners' arguments
        static int luaAdd(lua_State* L);              // (tag, fn), the event object protocol
        static int luaRemove(lua_State* L);           // (tag) -> bool
        static int luaHas(lua_State* L);              // (tag) -> bool
        static int luaToString(lua_State* L);

    private:
        struct Listener {
            int function = -2;      // LUA_NOREF: a method of the owner instead
            int owner = -2;         // the script instance that owns it (LUA_NOREF: none)
            std::string method;
            std::string tag;        // add(tag, fn) / RegisterEvent
            bool removed = false;   // a running invoke skips it
        };

        lua_State* state = nullptr;
        mutable std::vector<std::shared_ptr<Listener>> listeners;

        void release(Listener& listener) const;
        void compact() const;
        bool ownerReleased(lua_State* L, const Listener& listener) const;
        void dropReleasedOwners() const;
        void dropOwner(lua_State* L, int ownerIndex);
        // owner -> the Events (weak) holding its listeners, for releaseOwner
        static void trackOwner(lua_State* L, int ownerIndex, int eventIndex);
    };

}

#endif // SCRIPT_EVENT_H
