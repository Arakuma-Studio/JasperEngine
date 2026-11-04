// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The backend factories (FileWatcher::create* picks one per platform). Engine-internal.

#ifndef JASPERENGINE_WATCH_BACKENDS_H
#define JASPERENGINE_WATCH_BACKENDS_H

#include "FileWatcher.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define JASPERENGINE_WATCH_FSEVENTS 1
#endif
#endif

namespace jasperengine::watch {

    // Locks `mutex` within `timeout` (false: it did not). A try_lock loop, not std::timed_mutex:
    // libstdc++'s try_lock_for locks through pthread_mutex_clocklock, which ThreadSanitizer does
    // not intercept, so every unlock after it is reported as an unlock of an unlocked mutex
    inline bool lockWithin(std::mutex& mutex, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!mutex.try_lock()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    std::unique_ptr<FileWatcher> createInotifyBackend();
#endif
#if defined(_WIN32)
    std::unique_ptr<FileWatcher> createRdcwBackend();
#endif
#if defined(JASPERENGINE_WATCH_FSEVENTS)
    std::unique_ptr<FileWatcher> createFsEventsBackend();
    // Tests: the next `count` streams report nothing, as one fseventsd does not serve
    JASPERENGINE_API void setFsEventsDeadStreamsForTesting(int count);
#endif
    std::unique_ptr<FileWatcher> createPollingBackend(std::chrono::milliseconds interval);
    std::unique_ptr<FileWatcher> createNullBackend();

}

#endif // JASPERENGINE_WATCH_BACKENDS_H
