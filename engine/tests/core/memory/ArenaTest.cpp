// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Arena and ScratchScope (S06 AC9). Ported from OpenEngine's Modules/Memory/Tests/
// ArenaTests.cpp and ScratchScopeTests.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "jobs/JobSystem.h"
#include "jobs/Primitives.h"
#include "memory/Arena.h"
#include "memory/ScratchScope.h"

#include "AllocationCounter.h"
#include "JobsTestUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::memory;

namespace{
    bool isAligned(const void* pointer, size_t alignment){
        return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
    }

    bool findEntry(const std::string& name, StatsEntry& out){
        for (StatsEntry& entry : snapshot()){
            if (entry.name == name){
                out = entry;
                return true;
            }
        }
        return false;
    }

    uint64_t blockCount(const std::string& name){
        StatsEntry entry;
        EXPECT_TRUE(findEntry(name, entry)) << "no arena registered as " << name;
        return entry.count;
    }

    struct Tracked {
        static int live;
        static int destroyed;
        int value = 0;
        explicit Tracked(int v): value(v){ live++; }
        ~Tracked(){ live--; destroyed++; }
    };
    int Tracked::live = 0;
    int Tracked::destroyed = 0;

    // one frame's worth of mixed allocations
    void fillOneFrame(Arena& arena){
        const size_t sizes[3] = {16, 64, 256};
        for (int allocation = 0; allocation < 240; allocation++)
            ASSERT_NE(arena.allocate(sizes[allocation % 3]), nullptr);
    }
}

TEST(ArenaTests, SingleAllocStoresValueAndAligns){
    Arena arena("Tests.Arena.SingleAlloc");
    int* integer = arena.allocate<int>(42);
    double* real = arena.allocate<double>(3.5);
    EXPECT_EQ(*integer, 42);
    EXPECT_DOUBLE_EQ(*real, 3.5);
    EXPECT_TRUE(isAligned(integer, alignof(int)));
    EXPECT_TRUE(isAligned(real, alignof(double)));
    EXPECT_NE((void*)integer, (void*)real);
}

TEST(ArenaTests, AllocateArrayValueInitializesAndIsContiguous){
    Arena arena("Tests.Arena.Array");
    uint32_t* values = arena.allocateArray<uint32_t>(16);
    for (size_t i = 0; i < 16; i++)
        EXPECT_EQ(values[i], 0u);
    for (size_t i = 0; i < 16; i++)
        values[i] = (uint32_t)(i * 7);
    EXPECT_EQ(values[15], 15u * 7u);
}

TEST(ArenaTests, CopyArrayPreservesValues){
    Arena arena("Tests.Arena.Copy");
    const std::array<int, 5> source = {1, 2, 3, 4, 5};
    int* copy = arena.copyArray(source.data(), source.size());
    EXPECT_NE(copy, source.data());
    for (size_t i = 0; i < source.size(); i++)
        EXPECT_EQ(copy[i], source[i]);
}

TEST(ArenaTests, GrowthChainsBlocksWithoutInvalidatingPointers){
    Arena arena("Tests.Arena.Growth", 256);
    std::vector<int*> pointers;
    for (int value = 0; value < 2000; value++)
        pointers.push_back(arena.allocate<int>(value));
    EXPECT_GT(blockCount("Tests.Arena.Growth"), 1u);
    for (int value = 0; value < 2000; value++)
        ASSERT_EQ(*pointers[(size_t)value], value);
    const std::set<int*> unique(pointers.begin(), pointers.end());
    EXPECT_EQ(unique.size(), pointers.size());
}

TEST(ArenaTests, ResetRewindsAndKeepsTheHighWaterCapacity){
    Arena arena("Tests.Arena.ResetKeeps", 256);
    for (int value = 0; value < 2000; value++)
        arena.allocate<int>(value);
    const size_t usedAfterFill = arena.bytesUsed();
    EXPECT_GT(usedAfterFill, 0u);
    arena.reset();
    EXPECT_EQ(arena.bytesUsed(), 0u);
    EXPECT_GE(arena.bytesReserved(), usedAfterFill);
    const uint64_t blocksAfterReset = blockCount("Tests.Arena.ResetKeeps");
    for (int value = 0; value < 2000; value++)
        arena.allocate<int>(value);
    EXPECT_EQ(blockCount("Tests.Arena.ResetKeeps"), blocksAfterReset);
}

TEST(ArenaTests, AllocateRunsDestructorsOnReset){
    Tracked::live = 0;
    Tracked::destroyed = 0;
    {
        Arena arena("Tests.Arena.Destructors");
        for (int value = 0; value < 10; value++)
            arena.allocate<Tracked>(value);
        EXPECT_EQ(Tracked::live, 10);
        EXPECT_EQ(arena.marker().destructorCount, 10u);
        arena.reset();
        EXPECT_EQ(Tracked::live, 0);
        EXPECT_EQ(Tracked::destroyed, 10);
    }
    EXPECT_EQ(Tracked::destroyed, 10) << "destroying the arena after reset destroyed an object twice";
}

TEST(ArenaTests, DestructorsRunOnArenaDestruction){
    Tracked::live = 0;
    Tracked::destroyed = 0;
    {
        Arena arena("Tests.Arena.Destruction");
        arena.allocate<Tracked>(1);
        arena.allocate<Tracked>(2);
    }
    EXPECT_EQ(Tracked::live, 0);
    EXPECT_EQ(Tracked::destroyed, 2);
}

TEST(ArenaTests, RewindRunsOnlyTheDestructorsAboveTheMarker){
    Tracked::live = 0;
    Tracked::destroyed = 0;
    Arena arena("Tests.Arena.RewindMarker");
    arena.allocate<Tracked>(1);
    arena.allocate<Tracked>(2);
    const Arena::Marker mark = arena.marker();
    const size_t usedAtMarker = arena.bytesUsed();
    arena.allocate<Tracked>(3);
    arena.allocate<Tracked>(4);
    arena.allocate<Tracked>(5);
    arena.rewind(mark);
    EXPECT_EQ(Tracked::destroyed, 3);
    EXPECT_EQ(Tracked::live, 2);
    EXPECT_EQ(arena.bytesUsed(), usedAtMarker);
    arena.reset();
    EXPECT_EQ(Tracked::destroyed, 5);
}

TEST(ArenaTests, OverAlignmentOf4096IsServedAcrossBlocks){
    Arena arena("Tests.Arena.Align4096", 64 * 1024);
    for (int allocation = 0; allocation < 40; allocation++){
        arena.allocate(1);
        void* aligned = arena.allocate(3000, 4096);
        ASSERT_TRUE(isAligned(aligned, 4096)) << "allocation " << allocation;
    }
    EXPECT_GT(blockCount("Tests.Arena.Align4096"), 1u);
}

TEST(ArenaTests, ResetCoalescesThenAllocatesNothingPerFrame){
    Arena arena("Tests.Arena.Coalesce", 4096);
    fillOneFrame(arena);
    ASSERT_GT(blockCount("Tests.Arena.Coalesce"), 1u);
    arena.reset();
    EXPECT_EQ(blockCount("Tests.Arena.Coalesce"), 1u);
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope frames;
    for (int frame = 0; frame < 8; frame++){
        fillOneFrame(arena);
        arena.reset();
    }
    EXPECT_EQ(frames.count(), 0u) << "a steady frame on a coalesced arena reached the heap";
}

TEST(ArenaTests, ArenaAllocatorOnAWarmArenaMakesNoHeapAllocation){
    Arena arena("Tests.Arena.Allocator");
    arena.allocate(1);
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope scope;
    {
        std::vector<int, ArenaAllocator<int>> values{ArenaAllocator<int>(arena)};
        for (int value = 0; value < 1000; value++)
            values.push_back(value);
        EXPECT_EQ(values[999], 999);
    }
    EXPECT_EQ(scope.count(), 0u);
}

TEST(ArenaTests, TheSnapshotReportsTheArenaUnderItsName){
    Arena arena("Tests.Arena.Snapshot", 4096);
    arena.allocate(1000);
    StatsEntry entry;
    ASSERT_TRUE(findEntry("Tests.Arena.Snapshot", entry));
    EXPECT_EQ(entry.kind, StatsKind::Arena);
    EXPECT_EQ(entry.residency, Residency::Cpu);
    EXPECT_EQ(entry.current, 1000u);
    EXPECT_EQ(entry.capacity, 4096u);
    EXPECT_EQ(entry.count, 1u);
    for (int allocation = 0; allocation < 8; allocation++)
        arena.allocate(64);
    const size_t peak = arena.bytesUsed();
    arena.reset();
    ASSERT_TRUE(findEntry("Tests.Arena.Snapshot", entry));
    EXPECT_EQ(entry.current, 0u);
    EXPECT_EQ(entry.peak, peak) << "the high water is the most in use before the reset";
}

TEST(ArenaTests, TheHighWaterRecordsThePeakBeforeARewind){
    Arena arena("Tests.Arena.RewindHighWater", 4096);
    const Arena::Marker start = arena.marker();
    arena.allocate(1000);
    arena.allocate(2000);
    const size_t peak = arena.bytesUsed();
    arena.rewind(start);
    StatsEntry entry;
    ASSERT_TRUE(findEntry("Tests.Arena.RewindHighWater", entry));
    EXPECT_EQ(entry.current, 0u);
    EXPECT_EQ(entry.peak, peak);
}

TEST(FrameArenaTests, EachFrameInFlightKeepsItsMemory){
    FrameArena frames("Tests.FrameArena");
    int* first = frames.current().allocate<int>(1);
    frames.beginFrame();
    int* second = frames.current().allocate<int>(2);
    EXPECT_EQ(*first, 1) << "the previous frame's memory survives one beginFrame";
    frames.beginFrame();
    EXPECT_EQ(frames.current().bytesUsed(), 0u) << "the arena two frames back was reset";
    EXPECT_EQ(*second, 2);
}

// ---- ScratchScope

TEST(ScratchScopeTests, NestedScopesRewindToTheirOwnMarks){
    ScratchScope outer;
    int* a = outer.allocateArray<int>(16);
    a[0] = 11;
    {
        ScratchScope inner;
        int* b = inner.allocateArray<int>(1024);
        b[1023] = 22;
        EXPECT_NE((void*)a, (void*)b);
    }
    int* c = outer.allocateArray<int>(4);
    c[0] = 33;
    EXPECT_EQ(a[0], 11);
    EXPECT_EQ(c[0], 33);
}

TEST(ScratchScopeTests, AScopeOpenAcrossAHelpingWaitSurvivesTheInnerJobsScopes){
    // a wait runs other jobs inline on this thread; their scopes nest inside this one
    jobs::JobSystem system(testsupport::configFor(0));
    ScratchScope outer;
    int* mine = outer.allocateArray<int>(64);
    for (int i = 0; i < 64; i++)
        mine[i] = i;
    jobs::Task<int> task = system.submit([]{
        ScratchScope scope;
        int* theirs = scope.allocateArray<int>(4096);
        theirs[4095] = 7;
        return theirs[4095];
    });
    EXPECT_EQ(task.get(), 7);
    system.parallelFor(32, [](size_t){
        ScratchScope scope;
        scope.allocateArray<char>(256)[0] = 1;
    });
    for (int i = 0; i < 64; i++)
        ASSERT_EQ(mine[i], i);
}

TEST(ScratchScopeTests, ScopesNestToDepth64){
    std::function<int(int)> nest = [&](int depth) -> int {
        ScratchScope scope;
        int* value = scope.allocate<int>(depth);
        const int below = depth < 64 ? nest(depth + 1) : 0;
        return *value + below;
    };
    EXPECT_EQ(nest(1), 64 * 65 / 2);
}

TEST(ScratchScopeTests, OutOfOrderGrowthIsCorrectAndCounted){
    const uint64_t before = ScratchScope::outOfOrderCount();
    ScratchScope outer;
    int* fromOuter = nullptr;
    {
        ScratchScope inner;
        inner.allocateArray<int>(8)[0] = 1;
        fromOuter = outer.allocateArray<int>(8);   // not the innermost: served aside
        fromOuter[7] = 5;
    }
    EXPECT_EQ(fromOuter[7], 5) << "an outer scope's allocation survives the inner close";
    EXPECT_EQ(ScratchScope::outOfOrderCount(), before + 1);
}

TEST(ScratchScopeTests, SteadyUseAllocatesNothing){
    for (int warm = 0; warm < 4; warm++){
        ScratchScope scope;
        scope.allocateArray<char>(32 * 1024);
    }
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope counting;
    for (int i = 0; i < 1000; i++){
        ScratchScope scope;
        scope.allocateArray<char>(32 * 1024)[0] = 1;
        ScratchScope inner;
        inner.allocateArray<int>(100)[0] = 2;
    }
    EXPECT_EQ(counting.count(), 0u);
}

TEST(ScratchScopeTests, ASpikeIsReleasedAfterTheTrimWindow){
    auto spikeArenas = []{
        int count = 0;
        for (const StatsEntry& entry : snapshot()){
            if (entry.name.rfind("Scratch.thread-", 0) == 0 && entry.capacity >= 8 * 1024 * 1024)
                count++;
        }
        return count;
    };
    std::thread([&]{
        {
            ScratchScope spike;
            spike.allocateArray<char>(8 * 1024 * 1024)[0] = 1;
        }
        EXPECT_EQ(spikeArenas(), 1);
        // the spike's window, then two windows of a small steady need: the block is released
        for (int i = 0; i < 3100; i++){
            ScratchScope scope;
            scope.allocateArray<char>(1024)[0] = 1;
        }
        EXPECT_EQ(spikeArenas(), 0);
    }).join();
}

TEST(ScratchScopeTests, EachThreadHasItsOwnArena){
    jobs::Event written;
    std::atomic<int> arrivals{0};
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int t = 0; t < 4; t++){
        threads.emplace_back([&, t]{
            ScratchScope scope;
            int* values = scope.allocateArray<int>(10000);
            for (int i = 0; i < 10000; i++)
                values[i] = t;
            if (arrivals.fetch_add(1) == 3)
                written.set();
            written.wait();
            bool same = true;
            for (int i = 0; i < 10000; i++)
                same = same && values[i] == t;
            if (same)
                ok++;
        });
    }
    for (auto& thread : threads)
        thread.join();
    EXPECT_EQ(ok.load(), 4);
}
