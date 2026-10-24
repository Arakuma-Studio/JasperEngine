// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The global operator new of jasperengine-core-tests, counting each thread's allocations for
// the zero-allocation tests (AllocationCounter.h)

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {
    thread_local size_t allocations = 0;

    void* allocate(size_t size) {
        allocations++;
        if (size == 0) size = 1;
        if (void* memory = std::malloc(size)) return memory;
        throw std::bad_alloc();
    }

    void* allocateAligned(size_t size, size_t alignment) {
        allocations++;
        if (size == 0) size = 1;
        void* memory = nullptr;
#if defined(_WIN32)
        memory = _aligned_malloc(size, alignment);
#else
        if (posix_memalign(&memory, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0) memory = nullptr;
#endif
        if (!memory) throw std::bad_alloc();
        return memory;
    }

    void releaseAligned(void* memory) {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }
}

size_t jasperengine::test::threadAllocations() {
    return allocations;
}

double jasperengine::test::perfSlack() {
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    return 0.0;
#else
    const char* value = std::getenv("JASPER_TEST_PERF_SLACK");
    const double slack = value ? std::atof(value) : 1.0;
    return slack > 0.0 ? slack : 1.0;
#endif
}

void* operator new(size_t size) { return allocate(size); }
void* operator new[](size_t size) { return allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }
void* operator new(size_t size, std::align_val_t alignment) { return allocateAligned(size, static_cast<size_t>(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return allocateAligned(size, static_cast<size_t>(alignment)); }
void operator delete(void* memory, std::align_val_t) noexcept { releaseAligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { releaseAligned(memory); }
void operator delete(void* memory, size_t, std::align_val_t) noexcept { releaseAligned(memory); }
void operator delete[](void* memory, size_t, std::align_val_t) noexcept { releaseAligned(memory); }

TEST(AllocationCounter, CountsTheCallingThreadsAllocations) {
    using jasperengine::test::ScopedAllocationCount;
    ScopedAllocationCount count;
    auto value = std::make_unique<std::string>(100, 'x');
    EXPECT_GE(count.count(), 1u);
    ScopedAllocationCount none;
    volatile int sum = 0;
    for (int i = 0; i < 10; i++) sum = sum + i;
    EXPECT_EQ(none.count(), 0u);
}
