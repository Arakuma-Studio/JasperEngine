// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Counts the process's heap allocations. The plain global operator new/delete are replaced here
// with malloc/free (libstdc++'s own call malloc from inside the shared library, past the
// linker's wrap), so they are counted at malloc, wrapped by the linker (--wrap=malloc, calloc,
// realloc: tests/core/CMakeLists.txt); the aligned forms, which the job system's slabs and the
// arenas use, are replaced and counted here. GNU ld only (JE_TEST_COUNTS_ALLOCATIONS):
// elsewhere countsAllocations() is false and the tests skip.
// Under ASan, whose allocator serves operator new itself (the plain forms are not replaced
// there), so that new never reaches malloc, every allocation is counted at ASan's allocator
// instead, through its malloc hook, and the wrappers above only forward.

#include "AllocationCounter.h"

#include <cstdlib>

#if defined(JE_TEST_COUNTS_ALLOCATIONS)

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <execinfo.h>

#if defined(JASPERENGINE_SANITIZE_ADDRESS)
// libasan's allocator hook (compiler-rt's sanitizer/allocator_interface.h, which not every
// toolchain installs): called for each allocation and free of the program
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*mallocHook)(const volatile void*, std::size_t),
                                                         void (*freeHook)(const volatile void*));
#endif

namespace{
    std::atomic<uint64_t> allocations{0};
    std::atomic<uint64_t> allocatedBytes{0};
    std::atomic<uint64_t> largestBytes{0};
    // static TLS of the executable: reading it never allocates
    thread_local uint64_t threadCount = 0;

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

    void count(std::size_t bytes){
        allocations.fetch_add(1, std::memory_order_relaxed);
        allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
        uint64_t largest = largestBytes.load(std::memory_order_relaxed);
        while (largest < bytes && !largestBytes.compare_exchange_weak(largest, bytes, std::memory_order_relaxed)) {}
        threadCount++;
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

#if defined(JASPERENGINE_SANITIZE_ADDRESS)
    // every allocation, whatever its entry point, reaches the sanitizer's allocator
    constexpr bool countAtWrappers = false;
    void onAllocate(const volatile void*, std::size_t bytes){ count(bytes); }
    void onFree(const volatile void*){}
    [[maybe_unused]] const int hooksInstalled = __sanitizer_install_malloc_and_free_hooks(onAllocate, onFree);
#else
    constexpr bool countAtWrappers = true;
#endif

    void* alignedCounted(std::size_t bytes, std::size_t alignment){
        if (countAtWrappers) count(bytes);
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
        if (countAtWrappers) count(bytes);
        return __real_malloc(bytes);
    }

    void* __wrap_calloc(std::size_t items, std::size_t bytes){
        if (countAtWrappers) count(items * bytes);
        return __real_calloc(items, bytes);
    }

    void* __wrap_realloc(void* memory, std::size_t bytes){
        if (countAtWrappers) count(bytes);
        return __real_realloc(memory, bytes);
    }
}

// JE_TEST_BINARY_HAS_NEW: the binary already replaces the plain new and delete through malloc
// (the editor tests' ReflectResolveBenchTest counts its own), which the wrapper counts the same
#if !defined(JASPERENGINE_SANITIZE_ADDRESS) && !defined(JE_TEST_BINARY_HAS_NEW)
// counted at the malloc wrapper
void* operator new(std::size_t bytes){
    if (void* memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes){
    if (void* memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept{ std::free(memory); }
void operator delete[](void* memory) noexcept{ std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept{ std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept{ std::free(memory); }
#endif

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

    uint64_t totalAllocatedBytes(){
        return allocatedBytes.load(std::memory_order_relaxed);
    }

    uint64_t largestAllocation(){
        return largestBytes.load(std::memory_order_relaxed);
    }

    void resetLargestAllocation(){
        largestBytes.store(0, std::memory_order_relaxed);
    }

    bool countsAllocations(){
        return true;
    }

    uint64_t threadAllocations(){
        return threadCount;
    }

}

#else

namespace testsupport {

    AllocationCountScope::AllocationCountScope(): start(0){}
    AllocationCountScope::~AllocationCountScope() = default;
    uint64_t AllocationCountScope::count() const{ return start; }   // 0: nothing is counted
    uint64_t totalAllocations(){ return 0; }
    uint64_t totalAllocatedBytes(){ return 0; }
    uint64_t largestAllocation(){ return 0; }
    void resetLargestAllocation(){}
    bool countsAllocations(){ return false; }
    uint64_t threadAllocations(){ return 0; }

}

#endif

namespace testsupport {

    double perfSlack(){
    #if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
        return 0.0;
    #else
        const char* value = std::getenv("JASPER_TEST_PERF_SLACK");
        const double slack = value ? std::atof(value) : 0.0;
        return slack > 0.0 ? slack : 0.0;
    #endif
    }

}
