// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Compile as C with the crash target's include paths. On case-insensitive hosts a
// flat core/util include path lets Signal.h shadow the system signal.h, including
// through the C runtime headers used by Lua and the crash handler.
#include <signal.h>

int jasperengineSystemSignalHeader(void) {
    volatile sig_atomic_t value = SIGABRT;
    return value;
}
