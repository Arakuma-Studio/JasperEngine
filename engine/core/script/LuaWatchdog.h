// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUAWATCHDOG_H
#define LUAWATCHDOG_H

// The hang watchdog's view into the engine's Lua state (diag/HangWatchdog.h): while the main
// thread stalls inside Lua, a count hook (lua_sethook, which Lua allows from another context:
// engine/libs/lua/ldebug.c) names the running function in the report, and in editor Play, past
// Preferences > Diagnostics > Interrupt Runaway Lua In Play After, raises "script interrupted:
// <fn> ran for 10.0 s without returning (hang watchdog)" in it. A hook already set (an eval
// timeout, the debugger) is left alone. Games never interrupt (breakLuaAfterMs stays 0).

#include "Export.h"

namespace jasperengine::LuaWatchdog {

    // Engine::systemInit, once the Lua state exists
    JASPERENGINE_API void attach();

    // True once after the interrupt's error was raised: the error paths log it as a script
    // crash and leave Play running instead of handing it to the crash hook
    JASPERENGINE_API bool takeInterrupt();

}

#endif // LUAWATCHDOG_H
