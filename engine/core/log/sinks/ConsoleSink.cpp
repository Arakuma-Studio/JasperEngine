// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"

#include <cstdio>

using namespace jasperengine;

namespace {

    const char* editorPrefix(const LogRecord& record) {
        if (record.category == &logCategoryBuild()) return "[BUILD] ";
        switch (record.level) {
            case LogLevel::Trace: return "[TRACE] ";
            case LogLevel::Debug: return "[DEBUG] ";
            case LogLevel::Warn:  return "[WARNING] ";
            case LogLevel::Error: return "[ERROR] ";
            case LogLevel::Fatal: return "[FATAL] ";
            default:              return (record.flags & LogFlagSuccess) ? "[SUCCESS] " : "[INFO] ";
        }
    }

}

ConsoleSink::ConsoleSink(Style style): style(style) {
}

void ConsoleSink::write(const LogRecord& record) {
    // the streams of today's output: the game's warnings and errors, the editor's errors
    // and build output go to stderr
    const bool toError = style == Style::Game
        ? record.level >= LogLevel::Warn
        : (record.level >= LogLevel::Error || record.category == &logCategoryBuild());
    // one stream at a time, so lines keep their order when both go to the same terminal
    if (!pending.empty() && toError != pendingToError) {
        flush();
    }
    pendingToError = toError;
    pending += style == Style::Game ? logConsolePrefix(record.level) : editorPrefix(record);
    if (logShowsCategory(*record.category)) {
        pending += '[';
        pending += record.category->name;
        pending += "] ";
    }
    pending.append(record.text.data(), record.text.size());
    pending += '\n';
}

void ConsoleSink::flush() {
    if (pending.empty()) return;
    FILE* stream = pendingToError ? stderr : stdout;
    std::fwrite(pending.data(), 1, pending.size(), stream);
    std::fflush(stream);
    pending.clear();
}
