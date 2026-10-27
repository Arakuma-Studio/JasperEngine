// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"

#include <cstddef>
#include <mutex>
#include <string>

namespace jasperengine {

    class JASPERENGINE_API Backtrace {
    public:
        // Raw return addresses of the calling thread, the caller's caller first after skip
        // frames. No allocation, no symbols: cheap enough for every logged error. 0 where
        // unsupported (web).
        static size_t capture(void** frames, size_t maxFrames, size_t skip = 0);

        // One frame as text: "module(+0xoffset) symbol+0xdisplacement", or "module?+0x<address>"
        // when no loaded module holds the address (a script library unloaded since).
        // Allocates and may take the DbgHelp lock: not for signal handlers.
        static void describe(const void* address, std::string& out);

        // DbgHelp is single-threaded: every Sym* call in the process goes under this lock
        // (the logger's symbolization and the editor's crash handler). Exists everywhere,
        // used on Windows only.
        static std::timed_mutex& dbgHelpMutex();
        // One Windows symbol session for logging, crash reports and watchdog walks. Call
        // initializeSymbolsLocked only with dbgHelpMutex held; the unique process handle
        // avoids colliding with another component's DbgHelp session. Null/false elsewhere.
        static void* dbgHelpProcess();
        static bool initializeSymbolsLocked();
    };

}
