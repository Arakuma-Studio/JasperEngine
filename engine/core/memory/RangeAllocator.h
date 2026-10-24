// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::RangeAllocator (Modules/Memory/Include/Memory/
// RangeAllocator.h, Source/RangeAllocator.cpp), Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License. See NOTICE.md.

#ifndef MEMORY_RANGEALLOCATOR_H
#define MEMORY_RANGEALLOCATOR_H

#include "Export.h"
#include "memory/MemoryStats.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace jasperengine { namespace memory {

    // Hands out ranges of offsets into storage the caller owns (a GPU buffer, a vector of
    // slots), counted in the caller's units: a vertex, an index, a slot. It never touches
    // the storage.
    //
    //     memory::RangeAllocator ranges{"Particles.Vertices", memory::Residency::Gpu,
    //                                   sizeof(Vertex), capacity, 256};
    //     memory::RangeAllocator::Allocation range = ranges.allocate(count);
    //     if (!range.isValid()) { /* nothing fits: grow the storage or refuse */ }
    //     ranges.freeAfter(range, frame + framesInFlight);   // the GPU may still read it
    //     ranges.reclaimThrough(retiredFrame);                // once per frame
    //
    // - Good fit over two-level segregated bins (TLSF): the size's power of two, split in
    //   eight linear bins, with a bitmap per level. allocate takes the most recently freed
    //   range of the request's bin when it fits, else of the first larger non-empty bin, and
    //   splits it; free merges with both free neighbours. Both are constant time.
    // - Release never allocates: records and the deferred queue grow only inside allocate,
    //   so free, freeAfter and reclaimThrough are noexcept and allocation-free.
    // - Failure: an invalid Allocation when nothing fits (the allocator is then unchanged).
    // - Owner-serialized. Stats as StatsKind::Range, in bytes (units x unitBytes).
    class JASPERENGINE_API RangeAllocator {
    public:
        static constexpr uint32_t kNoNode = UINT32_MAX;

        // A range handed out, in units. Pass it back unchanged.
        struct Allocation {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint32_t node = kNoNode;

            bool isValid() const { return size != 0; }
        };

        struct Stats {
            uint64_t capacityUnits = 0;
            // live ranges, those waiting in freeAfter's queue included
            uint64_t usedUnits = 0;
            uint64_t peakUnits = 0;
            // the end of the highest range ever handed out; never decreases
            uint64_t endUnits = 0;
            uint64_t largestFreeUnits = 0;
            uint32_t freeRanges = 0;
            uint32_t pendingRanges = 0;
            // (free - largest free) / free; 0 when the free space is one range
            float fragmentation = 0.0f;
        };

        RangeAllocator(std::string_view statsName, Residency residency, uint64_t unitBytes,
                       uint64_t capacityUnits, uint32_t initialRangeCapacity);
        ~RangeAllocator();

        RangeAllocator(const RangeAllocator&) = delete;
        RangeAllocator& operator=(const RangeAllocator&) = delete;

        // Exactly `units`, or an invalid Allocation.
        Allocation allocate(uint64_t units);
        void free(Allocation allocation) noexcept;
        // Returns `allocation` at the first reclaimThrough(stamp) with stamp >= retireStamp.
        void freeAfter(Allocation allocation, uint64_t retireStamp) noexcept;
        void reclaimThrough(uint64_t safeStamp) noexcept;
        // Grows in place into the free range after it; false (unchanged) when it cannot.
        bool tryGrowInPlace(Allocation& allocation, uint64_t units);

        uint64_t getCapacity() const { return capacity; }
        uint64_t getUsed() const { return used; }
        bool isEmpty() const { return outstanding == 0; }
        Stats getStats() const;

    private:
        static constexpr uint32_t kSecondLevelBits = 3;
        static constexpr uint32_t kSecondLevelCount = 1u << kSecondLevelBits;
        static constexpr uint32_t kFirstLevelCount = 64 - kSecondLevelBits + 1;

        enum class NodeState : uint8_t { Unused, Free, Live, Pending };

        struct Node {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint32_t previousPhysical = kNoNode;
            uint32_t nextPhysical = kNoNode;
            uint32_t previousFree = kNoNode;
            uint32_t nextFree = kNoNode;
            NodeState state = NodeState::Unused;
        };

        struct PendingRange {
            uint64_t retireStamp = 0;
            uint32_t node = kNoNode;
        };

        struct Bin {
            uint32_t firstLevel;
            uint32_t secondLevel;
        };

        static Bin binOf(uint64_t units);
        uint32_t findFit(uint64_t units) const;
        void insertFree(uint32_t node);
        void removeFree(uint32_t node);
        void retireNode(uint32_t node);
        uint32_t takeNode();
        void releaseNode(uint32_t node) noexcept;
        bool isLiveAllocation(const Allocation& allocation) const;
        void publishStats() noexcept;

        std::vector<Node> nodes;
        std::vector<PendingRange> pending;
        std::array<uint32_t, kFirstLevelCount * kSecondLevelCount> freeHeads;
        std::array<uint8_t, kFirstLevelCount> secondLevelMaps{};
        uint64_t firstLevelMap = 0;
        uint32_t unusedHead = kNoNode;
        uint32_t freeRanges = 0;
        uint64_t capacity = 0;
        uint64_t unitBytes = 1;
        uint64_t used = 0;
        uint64_t peak = 0;
        uint64_t end = 0;
        uint64_t outstanding = 0;
        uint64_t earliestPendingStamp = UINT64_MAX;
        detail::StatsRecord* record;
    };

}}

#endif // MEMORY_RANGEALLOCATOR_H
