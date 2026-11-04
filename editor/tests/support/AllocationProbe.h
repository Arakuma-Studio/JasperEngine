// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor test binary's own operator new (AllocationProbe.cpp) counts the calling thread's
// allocations: how many, how many bytes, and the largest one, for tests that bound what code
// allocates (the reflection resolve path allocates nothing; a hostile .aseprite file asks for no
// more than R6 allows).

#pragma once

#include <cstddef>

namespace jasperengine::editor::test {

    struct ThreadAllocations {
        unsigned long long count = 0;
        unsigned long long bytes = 0;
        std::size_t largest = 0;
    };

    // since the thread started
    ThreadAllocations threadAllocations();
    // the largest one from now on (count and bytes keep counting)
    void resetLargestAllocation();

}
