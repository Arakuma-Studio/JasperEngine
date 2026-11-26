// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Log.h"
#include "util/CrashHook.h"
#include "util/Signal.h"

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace jasperengine {

    // Listener priorities of a CancellableEvent (and of Lua's Event): higher runs first, equal
    // ones in the order they were added
    struct EventPriority {
        static constexpr int32_t Min = -1000000;
        static constexpr int32_t Max = 1000000;
        // runs after the decision, in the order added: sees it, cannot change it
        static constexpr int32_t Monitor = INT32_MIN;
    };

    template<typename... Args>
    class CancellableEvent;

    // What a CancellableEvent's listener gets first. cancel() vetoes the change; nothing undoes it.
    class CancelEventArgs {
    public:
        CancelEventArgs(const CancelEventArgs&) = delete;
        CancelEventArgs& operator=(const CancelEventArgs&) = delete;

        // In the monitor pass: logged once per event object, and ignored
        void cancel() {
            if (!monitor) {
                cancelled = true;
                return;
            }
            if (!*monitorErrorLogged) {
                *monitorErrorLogged = true;
                Log::error("%s: a monitor listener cannot cancel; give it a priority instead", eventName);
            }
        }

        bool isCancelled() const {
            return cancelled;
        }

        bool isMonitorPass() const {
            return monitor;
        }

    private:
        template<typename...> friend class CancellableEvent;
        CancelEventArgs() = default;

        bool cancelled = false;
        bool monitor = false;
        // the event went away during its emit: its flag is no longer there
        bool orphaned = false;
        bool orphanErrorLogged = false;
        const char* eventName = nullptr;
        bool* monitorErrorLogged = nullptr;
        // the emit of the same event this one is nested in
        CancelEventArgs* outer = nullptr;
    };

    // An event that asks its listeners before a change: any of them may cancel() it, and the emitter
    // must not apply the change when emit returns true. Listeners run by priority (EventPriority),
    // highest first; after a cancel the deciding listeners left are skipped, then the monitor
    // listeners (EventPriority::Monitor) run with the decision final. Built on Signal's core
    // (detail::SlotList: the same re-entrancy rules, a Subscription per listener, owner keys), so a
    // listener runs as FunctionSubscribe::call runs it: under the crash guard while a CrashHook is
    // set (a faulting one is removed and reported), else a C++ exception is logged. Externally
    // synchronized, as Signal. The engine adoption rule is in docs/core-utilities.md.
    template<typename... Args>
    class CancellableEvent {
    public:
        using Listener = std::function<void(CancelEventArgs&, Args...)>;

        // name: what the monitor-cancel error names; a string that outlives the event (a literal)
        explicit CancellableEvent(const char* name): name(name) {}
        CancellableEvent(const CancellableEvent&) = delete;
        CancellableEvent& operator=(const CancellableEvent&) = delete;

        ~CancellableEvent() {
            for (CancelEventArgs* e = innermost; e; e = e->outer) {
                e->orphaned = true;
                e->orphanErrorLogged = monitorErrorLogged;
                e->monitorErrorLogged = &e->orphanErrorLogged;
            }
            if (state) state->orphan();
        }

        // priority: EventPriority::Min..Max, or EventPriority::Monitor. ownerKey (ownerKeyOf(this)
        // of a script) lets SubscriptionOwnerIndex::removeOwner find it
        [[nodiscard]] Subscription subscribe(Listener function, int32_t priority = 0, uint64_t ownerKey = 0) {
            if (!state) state = std::make_shared<List>();
            const uint64_t id = state->add(std::move(function), ownerKey, std::string(), priority);
            return Subscription(std::weak_ptr<detail::SlotStateBase>(state), id);
        }

        // True when a listener cancelled: the caller must not apply the change
        [[nodiscard]] bool emit(Args... args) {
            if (!state || state->size() == 0) return false;
            return emitSlow(args...);
        }

        bool hasListeners() const {
            return size() > 0;
        }

        size_t size() const {
            return state ? state->size() : 0;
        }

        size_t removeOwner(uint64_t ownerKey) {
            return state ? state->removeOwner(ownerKey) : 0;
        }

        void clear() {
            if (state) state->clear();
        }

    private:
        using List = detail::SlotList<void(CancelEventArgs&, Args...)>;
        using Slot = typename List::Slot;

        std::shared_ptr<List> state;
        const char* name;
        bool monitorErrorLogged = false;
        CancelEventArgs* innermost = nullptr;

        bool emitSlow(Args... args) {
            // the list, not the member: a listener may destroy this event
            List* slots = state.get();
            CancelEventArgs e;
            e.eventName = name;
            e.monitorErrorLogged = &monitorErrorLogged;
            e.outer = innermost;
            innermost = &e;
            const bool guarded = CrashHook::active();
            slots->invoke([&](Slot& slot) {
                if (slot.priority == EventPriority::Monitor) {
                    e.monitor = true;   // monitors sort last
                } else if (e.cancelled) {
                    return true;        // a decider after the veto
                }
                if (!guarded) {
                    try {
                        slot.function(e, args...);
                    } catch (...) {
                        CrashHook::logFailure(std::string(e.eventName) + " listener");
                    }
                } else {
                    runGuarded(*slots, slot, e, args...);
                }
                return true;
            });
            if (!e.orphaned) innermost = e.outer;
            return e.cancelled;
        }

        // under the crash guard (a CrashHook is set): a faulting listener is removed and reported
        static void runGuarded(List& slots, Slot& slot, CancelEventArgs& e, Args&... args) {
            auto call = [&]() { slot.function(e, args...); };
            using Call = decltype(call);
            std::string errorInfo;
            if (!CrashHook::runGuarded([](void* data) { (*static_cast<Call*>(data))(); }, &call, errorInfo)) {
                slots.removeSlot(slot.id);
                CrashHook::report(std::string(e.eventName) + " listener", errorInfo);
            }
        }
    };

}
