// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_TIMER_H
#define SCRIPT_TIMER_H

#include "ScriptScheduler.h"

namespace jasperengine {

    // Timers for C++ code that is not a script (the C++ side of Lua's Timer.after / Timer.every;
    // a ScriptBase script uses invoke / invokeRepeating, cancelled with it). Callbacks run on the
    // main thread in the update phase after onUpdate. An owner (any pointer) makes
    // ScriptScheduler::cancelOwner(owner) cancel them; unowned ones end at Play stop / shutdown.
    class Timer {
    public:
        static TaskId after(double delay, std::function<void()> fn, TimeMode mode = TimeMode::Scaled, const void* owner = nullptr){
            return ScriptScheduler::after(delay, std::move(fn), mode, owner);
        }
        static TaskId every(double interval, std::function<void()> fn, TimeMode mode = TimeMode::Scaled, const void* owner = nullptr,
                            int count = -1){
            return ScriptScheduler::every(interval, interval, std::move(fn), mode, owner, count);
        }
        static bool cancel(TaskId id){ return ScriptScheduler::cancel(id); }
        static bool isPending(TaskId id){ return ScriptScheduler::isPending(id); }
    };

}

#endif //SCRIPT_TIMER_H
