// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThreadCpuClock.h"

#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <time.h>
#endif

using namespace jasperengine;

namespace {
    std::atomic<uint64_t> threadCpuReads{0};
}

bool ThreadCpuClock::isAvailable(){
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

double ThreadCpuClock::nowMs(){
    threadCpuReads.fetch_add(1, std::memory_order_relaxed);
#if defined(__EMSCRIPTEN__)
    return -1.0;
#elif defined(_WIN32)
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        return -1.0;
    // 100 ns units; advanced at the scheduler tick (about 15.6 ms)
    const uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    const uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (double)(k + u) / 10000.0;
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

uint64_t ThreadCpuClock::readCount(){
    return threadCpuReads.load(std::memory_order_relaxed);
}
