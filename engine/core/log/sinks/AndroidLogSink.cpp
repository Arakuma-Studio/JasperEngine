// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(__ANDROID__)

#include "log/LogSinks.h"

#include <android/log.h>

using namespace jasperengine;

void AndroidLogSink::write(const LogRecord& record) {
    int priority = ANDROID_LOG_INFO;
    switch (record.level) {
        case LogLevel::Trace: priority = ANDROID_LOG_VERBOSE; break;
        case LogLevel::Debug: priority = ANDROID_LOG_DEBUG; break;
        case LogLevel::Warn:  priority = ANDROID_LOG_WARN; break;
        case LogLevel::Error: priority = ANDROID_LOG_ERROR; break;
        case LogLevel::Fatal: priority = ANDROID_LOG_FATAL; break;
        default: break;
    }
    scratch.clear();
    if (logShowsCategory(*record.category)) {
        scratch += '[';
        scratch += record.category->name;
        scratch += "] ";
    }
    scratch.append(record.text.data(), record.text.size());
    __android_log_write(priority, "JasperEngine", scratch.c_str());
}

#endif
