// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "memory/HeapTrim.h"

#include "Log.h"
#include "jobs/JobSystem.h"
#include "profiler/Profile.h"

#include <atomic>
#include <chrono>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <malloc.h>
#elif defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

using namespace jasperengine;

namespace {

    // 0 idle, 1 a trim queued or running, 2 one more after it
    std::atomic<int> trimState(0);
    std::atomic<uint64_t> trims(0);

    bool trimSupported(){
#if defined(__ANDROID__)
#if defined(M_PURGE)
        return true;
#else
        return false;
#endif
#elif (defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__)
        return true;
#else
        return false;
#endif
    }

    void runTrims(){
        do {
            const auto start = std::chrono::steady_clock::now();
            memory::releaseFreeHeap();
            trims++;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            Log::debug("Heap trim: %.1f ms", ms);
        } while (trimState.fetch_sub(1) == 2);
    }

}

bool memory::releaseFreeHeap(){
    JE_PROFILE_SCOPE("Memory.HeapTrim");
#if defined(__ANDROID__)
#if defined(M_PURGE)
    // mallopt is in libc from Android 8 (API 26); the player builds for older ones, so it is
    // looked up. M_PURGE is understood from Android 9 (API 28): before, the call does nothing
    using MalloptFunction = int (*)(int, int);
    static const MalloptFunction purge = reinterpret_cast<MalloptFunction>(dlsym(RTLD_DEFAULT, "mallopt"));
    return purge && purge(M_PURGE, 0) == 1;
#else
    return false;
#endif
#elif defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
    return true;
#elif defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
    return true;
#else
    return false;
#endif
}

void memory::requestHeapTrim(){
    if (!trimSupported()){
        return;
    }
    int state = trimState.load();
    while (true){
        if (state == 0){
            if (trimState.compare_exchange_weak(state, 1)){
                jobs::enqueueDetached([](){ runTrims(); }, jobs::Lane::Background, "heap-trim");
                return;
            }
        }else if (state == 1){
            if (trimState.compare_exchange_weak(state, 2)){
                return;
            }
        }else{
            return;
        }
    }
}

uint64_t memory::heapTrimCount(){
    return trims.load();
}
