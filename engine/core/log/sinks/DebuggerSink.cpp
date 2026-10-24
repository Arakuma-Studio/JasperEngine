// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(_WIN32)

#include "log/LogSinks.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jasperengine;

void DebuggerSink::write(const LogRecord& record) {
    if (!IsDebuggerPresent()) return;
    scratch = logConsolePrefix(record.level);
    if (logShowsCategory(*record.category)) {
        scratch += '[';
        scratch += record.category->name;
        scratch += "] ";
    }
    scratch.append(record.text.data(), record.text.size());
    scratch += '\n';
    OutputDebugStringA(scratch.c_str());
}

#endif
