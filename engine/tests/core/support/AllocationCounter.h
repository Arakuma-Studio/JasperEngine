// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_ALLOCATIONCOUNTER_H
#define TESTS_ALLOCATIONCOUNTER_H

// The test binaries replace the global operator new/delete with counting versions: an
// AllocationCountScope counts every heap allocation of the process (every thread) while it
// is open, so "0 allocations at steady state" can be checked through the whole job system.

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

}

#endif // TESTS_ALLOCATIONCOUNTER_H
