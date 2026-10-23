// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Log.h"
#include "log/Logger.h"

#include <stdarg.h>

using namespace jasperengine;

namespace {

    void logAt(LogLevel level, const char* fmt, va_list args) {
        LogCategory& category = logCategoryEngine();
        if (Logger::shouldLog(level, category)) {
            Logger::vwritef(level, category, nullptr, fmt, args);
        }
    }

}

void Log::print(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Info, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::verbose(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Trace, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::debug(const char *fmt, ...) {
    #ifndef NDEBUG
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Debug, fmt, arg_ptr);
    va_end(arg_ptr);
    #else
    (void)fmt;
    #endif
}

void Log::warn(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Warn, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::error(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Error, fmt, arg_ptr);
    va_end(arg_ptr);
}
