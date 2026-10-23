// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The backend factories (FileWatcher::create* picks one per platform). Engine-internal.

#ifndef JASPERENGINE_WATCH_BACKENDS_H
#define JASPERENGINE_WATCH_BACKENDS_H

#include "FileWatcher.h"

#include <chrono>
#include <memory>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define JASPERENGINE_WATCH_FSEVENTS 1
#endif
#endif

namespace jasperengine::watch {

#if defined(__linux__) && !defined(__ANDROID__)
    std::unique_ptr<FileWatcher> createInotifyBackend();
#endif
#if defined(_WIN32)
    std::unique_ptr<FileWatcher> createRdcwBackend();
#endif
#if defined(JASPERENGINE_WATCH_FSEVENTS)
    std::unique_ptr<FileWatcher> createFsEventsBackend();
#endif
    std::unique_ptr<FileWatcher> createPollingBackend(std::chrono::milliseconds interval);
    std::unique_ptr<FileWatcher> createNullBackend();

}

#endif // JASPERENGINE_WATCH_BACKENDS_H
