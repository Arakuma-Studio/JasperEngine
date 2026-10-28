// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Log.h"
#include "System.h"
#include <stdarg.h>
#include <stdio.h>

using namespace jasperengine;

namespace {
    // Through the System's log; without one (before Engine::systemInit, or code that runs
    // without the engine: editor CLI tools) to stderr, never an abort
    void emit(const int type, const char* fmt, va_list args) {
        if (System::hasInstance()) {
            System::instance().platformLog(type, fmt, args);
            return;
        }
        const char* priority = type == D_LOG_VERBOSE ? "VERBOSE" : type == D_LOG_DEBUG ? "DEBUG" : type == D_LOG_WARN ? "WARN" : type == D_LOG_ERROR ? "ERROR" : nullptr;
        if (priority) fprintf(stderr, "(%s): ", priority);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    }
}

void Log::print(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(0, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::verbose(const char *fmt, ...) {
	va_list arg_ptr;
	va_start(arg_ptr, fmt);
    emit(D_LOG_VERBOSE, fmt, arg_ptr);
	va_end(arg_ptr);
}

void Log::debug(const char *fmt, ...) {
    #ifndef NDEBUG
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_DEBUG, fmt, arg_ptr);
    va_end(arg_ptr);
    #endif
}

void Log::warn(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_WARN, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::error(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_ERROR, fmt, arg_ptr);
    va_end(arg_ptr);
}
