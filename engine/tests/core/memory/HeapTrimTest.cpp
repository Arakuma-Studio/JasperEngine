// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The heap trim after scene loads (mesh-memory spec 6.8): freed heap goes back to the system
// where the allocator can give it (glibc), and requests made while one runs are merged.

#include <gtest/gtest.h>

#include "jobs/JobSystem.h"
#include "memory/HeapTrim.h"
#include "support/JobsTestUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/api-level.h>
#include <malloc.h>
#endif

// where the allocator gives freed heap back (memory/HeapTrim.cpp: Android from 9, with M_PURGE)
#if (defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__) || (defined(__ANDROID__) && defined(M_PURGE))
#define HEAP_TRIM_TEST_SUPPORTED 1
#else
#define HEAP_TRIM_TEST_SUPPORTED 0
#endif

using namespace jasperengine;

namespace {

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
    // resident bytes of this process
    size_t residentBytes() {
        std::ifstream statm("/proc/self/statm");
        size_t pages = 0, resident = 0;
        statm >> pages >> resident;
        return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
    }
#endif

    bool waitForTrims(uint64_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (memory::heapTrimCount() < count && std::chrono::steady_clock::now() < deadline) {
            jobs::pump(5.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return memory::heapTrimCount() >= count;
    }

}

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
TEST(HeapTrim, ReturnsFreedPagesOnGlibc) {
    // 64 MiB of small blocks, then one block above them that stays: freeing the blocks leaves a
    // free hole inside the heap, which free() does not give back (only a free top does)
    constexpr size_t block = 4096;
    constexpr size_t count = (64u << 20) / block;
    std::vector<void*> blocks(count);
    for (void*& b : blocks) {
        b = std::malloc(block);
        std::memset(b, 1, block);
    }
    // (a size no free chunk of the process serves: it comes from the top of the heap, above the blocks)
    constexpr size_t aboveSize = 100003;
    void* above = std::malloc(aboveSize);
    std::memset(above, 1, aboveSize);
    for (void* b : blocks) std::free(b);
    const size_t before = residentBytes();

    EXPECT_TRUE(memory::releaseFreeHeap());
    const size_t after = residentBytes();
    // a sanitizer's allocator holds freed memory in its own quarantine, which malloc_trim does
    // not reach: the resident bytes mean nothing there
#if !JE_TEST_SANITIZED
    EXPECT_GE(before - std::min(before, after), size_t(32u << 20))
        << "resident before " << (before >> 20) << " MiB, after " << (after >> 20) << " MiB";
#else
    (void)before;
    (void)after;
#endif
    std::free(above);
}
#elif defined(__ANDROID__)
TEST(HeapTrim, PurgesFromAndroid9) {
    const bool purged = memory::releaseFreeHeap();
#if defined(M_PURGE)
    // mallopt(M_PURGE) is understood from Android 9 (API 28); before, it answers it did nothing
    if (android_get_device_api_level() >= 28) EXPECT_TRUE(purged);
    else EXPECT_FALSE(purged);
#else
    EXPECT_FALSE(purged);
#endif
}
#else
TEST(HeapTrim, NothingWhereTheAllocatorCannot) {
#if defined(__APPLE__)
    EXPECT_TRUE(memory::releaseFreeHeap());
#else
    EXPECT_FALSE(memory::releaseFreeHeap());
#endif
}
#endif

TEST(HeapTrim, RequestsMerge) {
#if HEAP_TRIM_TEST_SUPPORTED
    const uint64_t before = memory::heapTrimCount();
    for (int i = 0; i < 10; i++) memory::requestHeapTrim();
    ASSERT_TRUE(waitForTrims(before + 1));
    // the job may have run between requests: at most one more run per request made while it ran
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    jobs::pump(5.0);
    EXPECT_LE(memory::heapTrimCount() - before, 3u) << "ten requests in a row are one or two trims, not ten";
#else
    const uint64_t before = memory::heapTrimCount();
    memory::requestHeapTrim();
    EXPECT_EQ(memory::heapTrimCount(), before) << "no trim where the allocator cannot";
#endif
}
