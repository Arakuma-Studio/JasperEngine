// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Counts the process's heap allocations. The engine links ThorVG, which replaces the plain
// global operator new/delete (engine/libs/thorvg/src/renderer/tvgInitializer.cpp) with
// malloc/free, so the plain forms are counted at malloc, wrapped by the linker
// (--wrap=malloc, calloc, realloc: tests/core/CMakeLists.txt); the aligned forms, which the
// job system's slabs and the arenas use, are replaced here. Linux only, like the test binary.

#include "AllocationCounter.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <execinfo.h>

namespace{
    std::atomic<uint64_t> allocations{0};

    // JE_TEST_TRACE_ALLOCATIONS=1 prints the stack of each allocation counted inside an
    // AllocationCountScope (the first 20), to find what reached the heap
    std::atomic<int> openScopes{0};
    std::atomic<int> traced{0};
    thread_local bool tracing = false;

    bool traceEnabled(){
        static const bool enabled = [](){
            const char* value = std::getenv("JE_TEST_TRACE_ALLOCATIONS");
            return value && value[0] == '1';
        }();
        return enabled;
    }

    void count(){
        allocations.fetch_add(1, std::memory_order_relaxed);
        if (openScopes.load(std::memory_order_relaxed) > 0 && !tracing && traceEnabled() && traced.fetch_add(1) < 20){
            tracing = true;
            void* frames[24];
            const int depth = backtrace(frames, 24);
            backtrace_symbols_fd(frames, depth, 2);
            static const char separator[] = "----\n";
            fwrite(separator, 1, sizeof(separator) - 1, stderr);
            tracing = false;
        }
    }

    void* alignedCounted(std::size_t bytes, std::size_t alignment){
        count();
        void* memory = nullptr;
        if (posix_memalign(&memory, alignment < sizeof(void*) ? sizeof(void*) : alignment, bytes ? bytes : 1) != 0)
            return nullptr;
        return memory;
    }
}

extern "C" {
    void* __real_malloc(std::size_t bytes);
    void* __real_calloc(std::size_t count, std::size_t bytes);
    void* __real_realloc(void* memory, std::size_t bytes);

    void* __wrap_malloc(std::size_t bytes){
        count();
        return __real_malloc(bytes);
    }

    void* __wrap_calloc(std::size_t items, std::size_t bytes){
        count();
        return __real_calloc(items, bytes);
    }

    void* __wrap_realloc(void* memory, std::size_t bytes){
        count();
        return __real_realloc(memory, bytes);
    }
}

void* operator new(std::size_t bytes, std::align_val_t alignment){
    if (void* memory = alignedCounted(bytes, (std::size_t)alignment))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes, std::align_val_t alignment){
    if (void* memory = alignedCounted(bytes, (std::size_t)alignment))
        return memory;
    throw std::bad_alloc();
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept{
    return alignedCounted(bytes, (std::size_t)alignment);
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept{
    return alignedCounted(bytes, (std::size_t)alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept{ std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept{ std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept{ std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept{ std::free(memory); }
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept{ std::free(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept{ std::free(memory); }

namespace testsupport {

    AllocationCountScope::AllocationCountScope(): start(allocations.load(std::memory_order_seq_cst)){
        openScopes.fetch_add(1);
    }

    AllocationCountScope::~AllocationCountScope(){
        openScopes.fetch_sub(1);
    }

    uint64_t AllocationCountScope::count() const{
        return allocations.load(std::memory_order_seq_cst) - start;
    }

    uint64_t totalAllocations(){
        return allocations.load(std::memory_order_relaxed);
    }

}
