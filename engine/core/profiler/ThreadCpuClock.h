// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ThreadCpuClock_h
#define ThreadCpuClock_h

// The CPU time of the calling thread (docs/specs/release-performance-and-gpu-quirks.md R5, D3):
// what it computed, without its waits on the driver, locks and I/O. Read at most once a second
// (Linux and Android serve it with a system call). Windows on x64 counts the thread's cycles, its
// first read measuring the counter's rate over 2 ms; elsewhere on Windows it advances at the
// scheduler tick (about 15.6 ms).

#include "Export.h"

#include <cstdint>

namespace jasperengine {

    class JASPERENGINE_API ThreadCpuClock {
    public:
        // false on the web, which has no per-thread CPU clock
        static bool isAvailable();
        // the calling thread's user + kernel time, ms (< 0: unavailable)
        static double nowMs();
        // how many times nowMs ran (tests: the sampler off reads no clock)
        static uint64_t readCount();
    };

}

#endif /* ThreadCpuClock_h */
