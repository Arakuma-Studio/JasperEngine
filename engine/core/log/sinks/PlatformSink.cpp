// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"
#include "System.h"

#include <cstdarg>

using namespace jasperengine;

namespace {

    int platformLogType(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return D_LOG_VERBOSE;
            case LogLevel::Debug: return D_LOG_DEBUG;
            case LogLevel::Warn:  return D_LOG_WARN;
            case LogLevel::Error:
            case LogLevel::Fatal: return D_LOG_ERROR;
            default:              return 0;
        }
    }

    void forward(System& system, int type, const char* format, ...) {
        va_list args;
        va_start(args, format);
        system.platformLog(type, format, args);
        va_end(args);
    }

}

PlatformSink::PlatformSink(System& system): system(system) {
}

void PlatformSink::write(const LogRecord& record) {
    scratch.clear();
    if (logShowsCategory(*record.category)) {
        scratch += '[';
        scratch += record.category->name;
        scratch += "] ";
    }
    scratch.append(record.text.data(), record.text.size());
    forward(system, platformLogType(record.level), "%s", scratch.c_str());
}
