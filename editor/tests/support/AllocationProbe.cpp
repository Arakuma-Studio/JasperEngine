// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AllocationProbe.h"

#include <cstdlib>
#include <new>

namespace {
    // the calling thread's allocations through operator new (the test binary's own code)
    thread_local jasperengine::editor::test::ThreadAllocations probeAllocations;

    void* probeAllocate(std::size_t size) noexcept {
        probeAllocations.count++;
        probeAllocations.bytes += size;
        if (size > probeAllocations.largest) probeAllocations.largest = size;
        return std::malloc(size ? size : 1);
    }
}

jasperengine::editor::test::ThreadAllocations jasperengine::editor::test::threadAllocations() {
    return probeAllocations;
}

void jasperengine::editor::test::resetLargestAllocation() {
    probeAllocations.largest = 0;
}

void* operator new(std::size_t size) {
    if (void* memory = probeAllocate(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (void* memory = probeAllocate(size)) return memory;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return probeAllocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return probeAllocate(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
