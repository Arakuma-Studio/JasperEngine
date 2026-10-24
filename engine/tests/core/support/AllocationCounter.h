// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_ALLOCATIONCOUNTER_H
#define TESTS_ALLOCATIONCOUNTER_H

// The test binaries count heap allocations (AllocationCounter.cpp): an AllocationCountScope
// counts every allocation of the process (every thread) while it is open, so "0 allocations at
// steady state" can be checked through the whole job system; a ThreadAllocationCount counts
// only the calling thread's, for code that must not allocate while other threads (the logger's
// drain thread, workers) may.

#include <cstdint>

namespace testsupport {

    class AllocationCountScope {
    public:
        AllocationCountScope();
        ~AllocationCountScope();

        AllocationCountScope(const AllocationCountScope&) = delete;
        AllocationCountScope& operator=(const AllocationCountScope&) = delete;

        // allocations since the scope opened
        uint64_t count() const;

    private:
        uint64_t start;
    };

    // every allocation of the process so far
    uint64_t totalAllocations();

    // false where the test binary cannot count (no GNU ld --wrap): the zero-allocation tests skip
    bool countsAllocations();

    // the calling thread's allocations so far
    uint64_t threadAllocations();

    // The calling thread's allocations since construction: ThreadAllocationCount c; ...; c.count()
    class ThreadAllocationCount {
    public:
        ThreadAllocationCount(): start(threadAllocations()) {}
        uint64_t count() const { return threadAllocations() - start; }

    private:
        uint64_t start;
    };

    // How much slower the build server may be than the performance budgets (JASPER_TEST_PERF_SLACK,
    // default 1); 0 under a sanitizer, where budgets are only printed
    double perfSlack();

}

#endif // TESTS_ALLOCATIONCOUNTER_H
