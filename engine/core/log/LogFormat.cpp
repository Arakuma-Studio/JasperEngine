// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LogSinks.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace jasperengine;

const char* jasperengine::logConsolePrefix(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "(VERBOSE): ";
        case LogLevel::Debug: return "(DEBUG): ";
        case LogLevel::Warn:  return "(WARN): ";
        case LogLevel::Error: return "(ERROR): ";
        case LogLevel::Fatal: return "(FATAL): ";
        default:              return "";
    }
}

bool jasperengine::logShowsCategory(const LogCategory& category) {
    return &category != &logCategoryEngine() && &category != &logCategoryEditor() &&
           &category != &logCategoryLua() && &category != &logCategoryBuild();
}

const char* jasperengine::logFileLabel(const LogRecord& record) {
    if (record.category == &logCategoryBuild()) return "BUILD";
    switch (record.level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return (record.flags & LogFlagSuccess) ? "OK   " : "INFO ";
    }
}

void jasperengine::logFormatTimestamp(double wallTime, char out[20]) {
    const std::time_t seconds = static_cast<std::time_t>(std::floor(wallTime));
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    if (std::strftime(out, 20, "%Y-%m-%d %H:%M:%S", &parts) == 0) {
        out[0] = '\0';
    }
}
