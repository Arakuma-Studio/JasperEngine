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
    // the record is UTF-8: the debugger gets UTF-16, whatever the process's code page
    const int length = MultiByteToWideChar(CP_UTF8, 0, scratch.data(), static_cast<int>(scratch.size()), nullptr, 0);
    if (length <= 0) return;
    wideScratch.resize(static_cast<size_t>(length));
    MultiByteToWideChar(CP_UTF8, 0, scratch.data(), static_cast<int>(scratch.size()), wideScratch.data(), length);
    wideScratch.push_back(L'\0');
    OutputDebugStringW(wideScratch.c_str());
}

#endif
