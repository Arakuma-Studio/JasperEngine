// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThreadCpuClock.h"

#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_M_X64)
#include <intrin.h>
#include <chrono>
#endif
#elif !defined(__EMSCRIPTEN__)
#include <time.h>
#endif

using namespace jasperengine;

namespace {
    std::atomic<uint64_t> threadCpuReads{0};

#if defined(_WIN32) && defined(_M_X64)
    // QueryThreadCycleTime counts the thread's time-stamp counter ticks exactly, where GetThreadTimes
    // charges a whole scheduler tick (about 15.6 ms) to whichever thread the tick finds running.
    // The counter's rate is measured over 2 ms once; 0 when it is not invariant (a rate that
    // follows the core's clock is no time), and GetThreadTimes serves.
    double cyclesPerMs(){
        static const double rate = []{
            int info[4];
            __cpuid(info, 0x80000000);
            if ((unsigned)info[0] < 0x80000007u)
                return 0.0;
            __cpuid(info, 0x80000007);
            if (!(info[3] & (1 << 8)))
                return 0.0;
            using Clock = std::chrono::steady_clock;
            const Clock::time_point start = Clock::now();
            const uint64_t startTicks = __rdtsc();
            Clock::time_point now = start;
            while (now - start < std::chrono::milliseconds(2))
                now = Clock::now();
            const uint64_t ticks = __rdtsc() - startTicks;
            const double ms = std::chrono::duration<double, std::milli>(now - start).count();
            return ticks > 0 ? (double)ticks / ms : 0.0;
        }();
        return rate;
    }
#endif
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
#if defined(_M_X64)
    const double rate = cyclesPerMs();
    if (rate > 0.0){
        // never GetThreadTimes after a failed read: a delta across the two sources is no time
        ULONG64 cycles = 0;
        return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? (double)cycles / rate : -1.0;
    }
#endif
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
