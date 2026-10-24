// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The allocation counter itself (AllocationCounter.h): the zero-allocation tests are only as
// good as its counts

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>

TEST(AllocationCounter, CountsTheCallingThreadsAllocations) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    testsupport::ThreadAllocationCount count;
    auto value = std::make_unique<std::string>(100, 'x');
    EXPECT_GE(count.count(), 1u);
    testsupport::ThreadAllocationCount none;
    volatile int sum = 0;
    for (int i = 0; i < 10; i++) sum = sum + i;
    EXPECT_EQ(none.count(), 0u);
}

TEST(AllocationCounter, OtherThreadsDoNotCountForTheCaller) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    testsupport::AllocationCountScope process;
    testsupport::ThreadAllocationCount mine;
    // the body allocates twice (the string and its buffer); starting the thread allocates its
    // state once on this thread
    std::thread([]() { auto value = std::make_unique<std::string>(100, 'z'); }).join();
    EXPECT_GE(process.count(), 3u);
    EXPECT_LE(mine.count(), 1u) << "the other thread's allocations counted for the caller";
}
