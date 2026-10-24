// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_ALLOCATION_COUNTER_H
#define JASPER_TEST_ALLOCATION_COUNTER_H

#include <cstddef>

namespace jasperengine { namespace test {

    // Heap allocations (operator new) made by the calling thread: the core tests' binary
    // replaces the global operator new (support/AllocationCounterTest.cpp) to count them
    size_t threadAllocations();

    // The allocations the calling thread made during its lifetime: ScopedAllocationCount c; ...; c.count()
    class ScopedAllocationCount {
    public:
        ScopedAllocationCount(): start(threadAllocations()) {}
        size_t count() const { return threadAllocations() - start; }

    private:
        size_t start;
    };

    // How much slower the build server may be than the performance budgets (JASPER_TEST_PERF_SLACK,
    // default 1); 0 under a sanitizer, where budgets are not checked
    double perfSlack();

} }

#endif
