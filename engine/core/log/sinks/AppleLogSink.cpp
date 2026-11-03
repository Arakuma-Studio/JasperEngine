// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(__APPLE__)

#include "log/LogSinks.h"

#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>

using namespace jasperengine;

AppleLogSink::AppleLogSink() {
    subsystem = "JasperEngine";
    if (CFBundleRef bundle = CFBundleGetMainBundle()) {
        if (CFStringRef identifier = CFBundleGetIdentifier(bundle)) {
            char buffer[256];
            if (CFStringGetCString(identifier, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                subsystem = buffer;
            }
        }
    }
}

void AppleLogSink::write(const LogRecord& record) {
    void*& handle = logs[record.category->index];
    if (!handle) {
        handle = os_log_create(subsystem.c_str(), record.category->name);
    }
    os_log_t log = static_cast<os_log_t>(handle);
    os_log_type_t type = OS_LOG_TYPE_DEFAULT;
    switch (record.level) {
        case LogLevel::Trace:
        case LogLevel::Debug: type = OS_LOG_TYPE_DEBUG; break;
        case LogLevel::Info:  type = OS_LOG_TYPE_INFO; break;
        case LogLevel::Warn:  type = OS_LOG_TYPE_DEFAULT; break;
        case LogLevel::Error: type = OS_LOG_TYPE_ERROR; break;
        case LogLevel::Fatal: type = OS_LOG_TYPE_FAULT; break;
        default: break;
    }
    scratch.assign(record.text.data(), record.text.size());
    os_log_with_type(log, type, "%{public}s", scratch.c_str());
}

#endif
