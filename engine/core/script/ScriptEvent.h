// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_EVENT_H
#define SCRIPT_EVENT_H

#include "Export.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    class ScriptCancelEventArgs;

    // Lua's `Event()` (Unity's UnityEvent / C# event): listeners called by priority, highest
    // first, then in the order they were added (every priority 0 by default: add order;
    // Event.MONITOR runs last). A listener is a function, or a method of a script instance that
    // owns it (`ev:addListener(self, "onScore")`), dropped once that instance is released (its
    // GameObject destroyed or its scene unloaded). Lua only, main thread; the listeners are
    // registry references released with the Event. A CancellableEvent asks them first.
    class JASPERENGINE_API ScriptEvent {
    public:
        ScriptEvent();
        ~ScriptEvent();
        ScriptEvent(const ScriptEvent&) = delete;
        ScriptEvent& operator=(const ScriptEvent&) = delete;

        // RegisterEvent(self, ev, "method" [, tag] [, priority]): a function under a tag (one per
        // tag), owned by the script at ownerIndex (0: none); eventIndex is this Event's userdata
        void addTagged(lua_State* L, int eventIndex, const std::string& tag, int functionIndex, int ownerIndex, int32_t priority = 0);
        // The script instance at `ownerIndex` is released: every Event drops the listeners it
        // owns (their registry references would keep it alive)
        static void releaseOwner(lua_State* L, int ownerIndex);
        bool removeTag(const std::string& tag);
        bool hasTag(const std::string& tag) const;
        // the listeners (those of released owners are dropped first)
        int listenerCount() const;
        // The priority argument at `index`: 0 when none or nil, else an integer in
        // EventPriority::Min..Max or Event.MONITOR; anything else raises an error naming `caller`
        static int32_t checkPriority(lua_State* L, int index, const char* caller);
        // a listener at this priority runs after every other one, in the order added; on a
        // CancellableEvent it sees the decision and cannot change it (EventPriority::Monitor)
        static int monitorPriority();

        // Lua methods (argument 1 is the Event)
        static int luaAddListener(lua_State* L);      // (fn [, priority]) or (owner, "method" [, priority])
        static int luaRemoveListener(lua_State* L);   // (fn) or (owner, "method") -> bool
        static int luaRemoveAllListeners(lua_State* L);
        static int luaInvoke(lua_State* L);           // (...) the listeners' arguments; a CancellableEvent returns cancelled
        static int luaAdd(lua_State* L);              // (tag, fn [, priority]), the event object protocol
        static int luaRemove(lua_State* L);           // (tag) -> bool
        static int luaHas(lua_State* L);              // (tag) -> bool
        static int luaGetListeners(lua_State* L);     // -> {name, priority, monitor}[] in dispatch order
        static int luaCancel(lua_State* L);           // an error naming the listener's CancelEventArgs
        static int luaToString(lua_State* L);

    protected:
        explicit ScriptEvent(bool cancellable);
        static int dispatch(lua_State* L, ScriptEvent* self);

        const bool cancellable;
        bool invokedOnce = false;
        bool lastCancelled = false;
        std::string lastCancelledBy;

    private:
        friend class ScriptCancelEventArgs;

        struct Listener {
            int function = -2;      // LUA_NOREF: a method of the owner instead
            int owner = -2;         // the script instance that owns it (LUA_NOREF: none)
            std::string method;
            std::string tag;        // add(tag, fn) / RegisterEvent
            bool removed = false;   // a running invoke skips it
            int32_t priority = 0;
        };
        // a CancellableEvent's CancelEventArgs kept for reuse, one per nesting depth
        static constexpr uint32_t CACHED_ARGS = 16;

        lua_State* state = nullptr;
        // sorted by priority (highest first), then add order; nothing is erased or inserted
        // while an invoke runs (depth > 0): removals mark, additions wait in `pending`
        mutable std::vector<std::shared_ptr<Listener>> listeners;
        mutable std::vector<std::shared_ptr<Listener>> pending;
        uint32_t depth = 0;
        mutable bool dirty = false;
        int cachedArgs[CACHED_ARGS];
        // their userdata, which Lua never moves and the registry reference keeps alive: an invoke
        // skips LuaBridge's metatable walk to reach it
        ScriptCancelEventArgs* cachedArgsData[CACHED_ARGS];

        void release(Listener& listener) const;
        void compact() const;
        void insert(std::shared_ptr<Listener> listener);
        static void insertSorted(std::vector<std::shared_ptr<Listener>>& list, std::shared_ptr<Listener> listener);
        bool ownerReleased(lua_State* L, const Listener& listener) const;
        void dropReleasedOwners() const;
        void dropOwner(lua_State* L, int ownerIndex);
        ScriptCancelEventArgs* pushCancelArgs(lua_State* L);
        std::string listenerName(lua_State* L, const Listener& listener) const;
        // owner -> the Events (weak) holding its listeners, for releaseOwner
        static void trackOwner(lua_State* L, int ownerIndex, int eventIndex);
    };

    // Lua's `CancellableEvent()`: an Event whose listeners get a CancelEventArgs first and may
    // cancel() the change; invoke returns true when one did, and the emitter must not apply it.
    // After a cancel the deciding listeners left are skipped; Event.MONITOR listeners run once the
    // decision is final.
    class JASPERENGINE_API CancellableScriptEvent : public ScriptEvent {
    public:
        CancellableScriptEvent();
        // Event's dispatch, reached without LuaBridge's walk to the base class: returns cancelled
        static int luaInvoke(lua_State* L);
        // the listener that cancelled the latest completed invoke (named as getListeners names it);
        // nil when it went ahead or before the first invoke
        std::optional<std::string> cancelledBy() const;
    };

    // Lua's CancelEventArgs: what a CancellableEvent's listener gets first (one per dispatch):
    // e:cancel() vetoes the change, monitors read e.cancelled
    class JASPERENGINE_API ScriptCancelEventArgs {
    public:
        // whether a listener of this invoke cancelled it (sticky: nothing clears it)
        bool isCancelled() const {
            return cancelled;
        }
        static int luaCancel(lua_State* L);

    private:
        friend class ScriptEvent;
        bool cancelled = false;
        bool monitor = false;
        bool ended = true;
        const ScriptEvent* event = nullptr;
        const ScriptEvent::Listener* current = nullptr;
        std::string cancelledBy;
    };

}

#endif // SCRIPT_EVENT_H
