// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"

#include <functional>
#include <string>

namespace jasperengine {

    // Where a crashing subscriber or timer is reported: (its tag or timer name, what went
    // wrong). The editor sets it while Play runs (pause, then stop); while it is set, every
    // FunctionSubscribe callback and scheduler task runs under callWithCrashGuard, and a
    // faulting one is removed (cancelled). Lua errors in callbacks go here too.
    using CrashHandler = std::function<void(const std::string& tag, const std::string& errorInfo)>;

    class JASPERENGINE_API CrashHook {
    public:
        // nullptr removes it. Main thread.
        static void set(CrashHandler handler);
        static bool active() noexcept;
        // Calls the handler; false when none is set
        static bool report(const std::string& tag, const std::string& errorInfo);
        // Runs function(context) under the crash guard. False when it faulted, with
        // "NAME (description) [code=N]" in errorInfo.
        static bool runGuarded(void (*function)(void*), void* context, std::string& errorInfo);
    };

}
