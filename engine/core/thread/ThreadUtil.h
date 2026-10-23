// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef THREADUTIL_H
#define THREADUTIL_H

// Thread identity and policy shared by the job system (jobs/JobSystem.h), the asset I/O
// readers and every dedicated service thread: names, OS priority, the worker-count policy
// and whether the calling thread may block.

#include "Export.h"

#include <cstdint>
#include <functional>
#include <thread>

namespace jasperengine { namespace thread {

    // The platform classes of the worker-count policy (defaultWorkerCount).
    enum class PlatformClass : uint8_t {
        Desktop,        // Windows, Linux, macOS
        Android,
        IOS,
        WebThreads,     // Emscripten with pthreads
        WebSingle,      // Emscripten without pthreads
    };

    // OS scheduling hint of a thread. Compute workers run at Normal; reader and blocking
    // threads below it, so a read storm yields the CPU to the frame. No affinity is ever set.
    enum class Priority : uint8_t {
        Normal,
        BelowNormal,
    };

    // The platform this build runs on.
    JASPERENGINE_API PlatformClass currentPlatform();

    // The cores the policy counts on this platform: logical cores on desktop and iOS,
    // configured (not just online) cores on Android, navigator.hardwareConcurrency on web.
    JASPERENGINE_API int detectCoreCount();

    // The worker policy for `cores` on `platform`:
    //   Desktop       clamp(cores - 1, 1, 15)
    //   Android, iOS  clamp(cores - 1, 1, 4)
    //   WebThreads    clamp(cores - 2, 1, 4)
    //   WebSingle     0 (single-thread mode)
    JASPERENGINE_API int workerCountFor(PlatformClass platform, int cores);

    // workerCountFor(currentPlatform(), detectCoreCount()).
    JASPERENGINE_API int defaultWorkerCount();

    // The workers to start: `configured` when >= 0, else the env JE_JOB_WORKERS when set
    // (0 forces single-thread mode on any platform), else defaultWorkerCount(). A build
    // without threads always gets 0.
    JASPERENGINE_API int resolveWorkerCount(int configured);

    // Names the calling thread for debuggers and profilers ("je-worker-3"). Linux and
    // Android keep 15 characters; Apple can only name the calling thread, which is why
    // there is no setName(thread).
    JASPERENGINE_API void setCurrentName(const char* name);

    // Applies `priority` to the calling thread: SetThreadPriority on Windows, a nice value
    // 5 above the process's on Linux and Android, QOS_CLASS_USER_INITIATED (Normal) or
    // QOS_CLASS_UTILITY (BelowNormal) on Apple. Nothing on web.
    JASPERENGINE_API void setCurrentPriority(Priority priority);

    // False on a thread that must never park: the browser main thread, which services the
    // proxied calls of the other threads (and is the only thread of a single-thread web build).
    JASPERENGINE_API bool canBlock();

    // A small process-unique id of the calling thread (1, 2, ...), stable for its lifetime.
    JASPERENGINE_API uint32_t currentThreadIndex();

    // Starts a dedicated thread that names itself `name` and sets `priority` before
    // running `fn`. Every long-lived engine and editor thread outside the job system
    // (MCP transports, video decoders) starts through here.
    JASPERENGINE_API std::thread spawnNamed(const char* name, std::function<void()> fn,
                                            Priority priority = Priority::Normal);

}}

#endif // THREADUTIL_H
