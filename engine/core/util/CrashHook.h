// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"

#include <atomic>
#include <functional>
#include <string>

namespace jasperengine {

    // Where a crashing subscriber or timer is reported: (its tag or timer name, what went
    // wrong). The editor sets it while Play runs (pause, then stop); while it is set, every
    // FunctionSubscribe callback and scheduler task runs under callWithCrashGuard, and a
    // faulting one is removed (cancelled). Lua errors in callbacks go here too.
    using CrashHandler = std::function<void(const std::string& tag, const std::string& errorInfo)>;

    namespace detail {
        // whether a CrashHook is set: read on every event invoke (defined in CrashHook.cpp, one
        // variable for the whole process)
        extern JASPERENGINE_API std::atomic<bool> crashHookActive;
    }

    class JASPERENGINE_API CrashHook {
    public:
        // nullptr removes it. Main thread.
        static void set(CrashHandler handler);
        static bool active() noexcept {
            return detail::crashHookActive.load(std::memory_order_acquire);
        }
        // Calls the handler; false when none is set
        static bool report(const std::string& tag, const std::string& errorInfo);
        // Runs function(context) under the crash guard. False when it faulted, with
        // "NAME (description) [code=N]" in errorInfo, or threw a C++ exception, with
        // describeCurrentException's text (the exception is caught inside the guard).
        static bool runGuarded(void (*function)(void*), void* context, std::string& errorInfo);
        // Runs function(context) and catches any C++ exception: false with its description in
        // errorInfo
        static bool runCatching(void (*function)(void*), void* context, std::string& errorInfo);
        // "C++ exception <type>: <what>" of the exception being handled (call it in a catch)
        static std::string describeCurrentException();
        // A callback that threw while no hook is set (an exported game): the exception being
        // handled (call it in a catch) in the log, at most once a second per tag; a callback
        // throwing every frame is not described (rethrown, demangled) each time. Any thread.
        static void logFailure(const std::string& tag);
    };

}
