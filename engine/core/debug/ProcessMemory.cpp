// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// System::getProcessMemoryBytes, on its own: the platform headers it needs (windows.h,
// mach) stay out of the rest of the engine

#include "System.h"

#if defined(__EMSCRIPTEN__)
    #include <emscripten/heap.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    // K32GetProcessMemoryInfo under its own name, whatever the target Windows version
    #ifndef PSAPI_VERSION
        #define PSAPI_VERSION 2
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
#elif defined(__linux__) || defined(__ANDROID__)
    #include <cstdio>
    #include <unistd.h>
#endif

using namespace jasperengine;

uint64_t System::getProcessMemoryBytes(){
#if defined(__EMSCRIPTEN__)
    // the WebAssembly heap: what the page holds for the game
    return static_cast<uint64_t>(emscripten_get_heap_size());
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters = {};
    counters.cb = sizeof(counters);
    // K32*: kernel32 itself, no psapi.lib to link
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<uint64_t>(counters.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    // what the system counts against the app (Xcode's memory gauge, the iOS jetsam limit)
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<uint64_t>(info.phys_footprint);
    return 0;
#elif defined(__linux__) || defined(__ANDROID__)
    // resident pages: the second number of /proc/self/statm
    FILE* file = std::fopen("/proc/self/statm", "r");
    if (!file)
        return 0;
    unsigned long long size = 0, resident = 0;
    const int read = std::fscanf(file, "%llu %llu", &size, &resident);
    std::fclose(file);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (read != 2 || pageSize <= 0)
        return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(pageSize);
#else
    return 0;
#endif
}
