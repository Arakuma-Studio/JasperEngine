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

    // the bytes every allocation of the process asked for so far, and the largest one since the
    // last reset: what a "no whole-map buffer" test looks at (a realloc counts what it asks for)
    uint64_t totalAllocatedBytes();
    uint64_t largestAllocation();
    void resetLargestAllocation();

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

    // The factor wall-clock budgets are enforced with: JASPER_TEST_PERF_SLACK, set by the
    // benchmark run on the benchmark server (docs/testing.md). 0 when it is not set, and under a
    // sanitizer: the budgets are only printed, since a shared builder's load alone can miss them.
    double perfSlack();

}

#endif // TESTS_ALLOCATIONCOUNTER_H
