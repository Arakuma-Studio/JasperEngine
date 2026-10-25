// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(__EMSCRIPTEN__)

#include "log/LogSinks.h"

#include <emscripten/emscripten.h>

using namespace jasperengine;

void EmscriptenConsoleSink::write(const LogRecord& record) {
    int flags = EM_LOG_CONSOLE;
    if (record.level >= LogLevel::Error) {
        flags |= EM_LOG_ERROR;
    } else if (record.level == LogLevel::Warn) {
        flags |= EM_LOG_WARN;
    }
    // the game console's text, as printf gave it before
    scratch = logConsolePrefix(record.level);
    if (logShowsCategory(*record.category)) {
        scratch += '[';
        scratch += record.category->name;
        scratch += "] ";
    }
    scratch.append(record.text.data(), record.text.size());
    emscripten_log(flags, "%s", scratch.c_str());
}

#endif
