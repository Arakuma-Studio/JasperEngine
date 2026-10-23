// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::RangeAllocator (Modules/Memory/Source/RangeAllocator.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "RangeAllocator.h"

#include <algorithm>
#include <cassert>
#include <new>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

using namespace jasperengine;
using namespace jasperengine::memory;

namespace{
    // the index of the highest set bit of a non-zero value (std::bit_width - 1)
    inline uint32_t highestBit(uint64_t value){
#if defined(_MSC_VER)
        unsigned long index;
        _BitScanReverse64(&index, value);
        return (uint32_t)index;
#else
        return 63u - (uint32_t)__builtin_clzll(value);
#endif
    }

    // the index of the lowest set bit of a non-zero value (std::countr_zero)
    inline uint32_t lowestBit(uint64_t value){
#if defined(_MSC_VER)
        unsigned long index;
        _BitScanForward64(&index, value);
        return (uint32_t)index;
#else
        return (uint32_t)__builtin_ctzll(value);
#endif
    }

    // grows a vector's capacity geometrically to at least `wanted`
    template<class T>
    void reserveGeometric(std::vector<T>& vector, size_t wanted){
        if (vector.capacity() >= wanted)
            return;
        vector.reserve(std::max(wanted, vector.capacity() * 2));
    }
}

RangeAllocator::RangeAllocator(std::string_view statsName, Residency residency, uint64_t unitBytesValue,
                               uint64_t capacityUnits, uint32_t initialRangeCapacity):
    capacity(capacityUnits), unitBytes(unitBytesValue),
    record(detail::registerRecord(statsName.empty() ? std::string_view("RangeAllocator") : statsName, StatsKind::Range, residency)){
    assert(!statsName.empty() && "a RangeAllocator needs a stats name");
    assert(unitBytesValue != 0 && "a RangeAllocator's unit is at least one byte");
    freeHeads.fill(kNoNode);
    nodes.reserve(std::max<uint32_t>(initialRangeCapacity, 1));
    pending.reserve(initialRangeCapacity);
    if (capacityUnits != 0){
        Node whole;
        whole.offset = 0;
        whole.size = capacityUnits;
        whole.state = NodeState::Free;
        nodes.push_back(whole);
        insertFree(0);
    }
    publishStats();
}

RangeAllocator::~RangeAllocator(){
    detail::unregisterRecord(record);
}

RangeAllocator::Bin RangeAllocator::binOf(uint64_t units){
    if (units < kSecondLevelCount)
        return {0, (uint32_t)units};
    const uint32_t log2 = highestBit(units);
    return {log2 - (kSecondLevelBits - 1), (uint32_t)(units >> (log2 - kSecondLevelBits)) & (kSecondLevelCount - 1)};
}

uint32_t RangeAllocator::findFit(uint64_t units) const{
    // TLSF's good fit, constant time: the request's own bin when its most recently freed
    // range fits, else the first larger non-empty bin, where every range fits. Walking a bin
    // for the smallest fit costs a cache miss per range for less than an eighth of a size.
    const Bin own = binOf(units);
    const uint32_t ownHead = freeHeads[own.firstLevel * kSecondLevelCount + own.secondLevel];
    if (ownHead != kNoNode && nodes[ownHead].size >= units)
        return ownHead;

    const uint32_t larger = own.secondLevel + 1;
    const uint32_t sameLevel = larger < kSecondLevelCount ? (secondLevelMaps[own.firstLevel] & (~0u << larger)) : 0u;
    Bin found{own.firstLevel, 0};
    if (sameLevel != 0){
        found.secondLevel = lowestBit(sameLevel);
    }else{
        const uint32_t nextLevel = own.firstLevel + 1;
        const uint64_t levels = nextLevel < 64 ? (firstLevelMap & (~0ull << nextLevel)) : 0ull;
        if (levels == 0){
            // nothing larger: another range of the request's own bin may still fit (rare,
            // near full; refusing then would fail a request the space can hold)
            for (uint32_t node = ownHead; node != kNoNode; node = nodes[node].nextFree){
                if (nodes[node].size >= units)
                    return node;
            }
            return kNoNode;
        }
        found.firstLevel = lowestBit(levels);
        found.secondLevel = lowestBit((uint64_t)secondLevelMaps[found.firstLevel]);
    }
    return freeHeads[found.firstLevel * kSecondLevelCount + found.secondLevel];
}

void RangeAllocator::insertFree(uint32_t node){
    Node& entry = nodes[node];
    const Bin bin = binOf(entry.size);
    uint32_t& head = freeHeads[bin.firstLevel * kSecondLevelCount + bin.secondLevel];
    entry.state = NodeState::Free;
    entry.previousFree = kNoNode;
    entry.nextFree = head;
    if (head != kNoNode)
        nodes[head].previousFree = node;
    head = node;
    secondLevelMaps[bin.firstLevel] |= (uint8_t)(1u << bin.secondLevel);
    firstLevelMap |= 1ull << bin.firstLevel;
    freeRanges++;
}

void RangeAllocator::removeFree(uint32_t node){
    Node& entry = nodes[node];
    if (entry.nextFree != kNoNode)
        nodes[entry.nextFree].previousFree = entry.previousFree;
    if (entry.previousFree != kNoNode){
        nodes[entry.previousFree].nextFree = entry.nextFree;
    }else{
        const Bin bin = binOf(entry.size);
        uint32_t& head = freeHeads[bin.firstLevel * kSecondLevelCount + bin.secondLevel];
        head = entry.nextFree;
        if (head == kNoNode){
            secondLevelMaps[bin.firstLevel] &= (uint8_t)~(1u << bin.secondLevel);
            if (secondLevelMaps[bin.firstLevel] == 0)
                firstLevelMap &= ~(1ull << bin.firstLevel);
        }
    }
    entry.previousFree = kNoNode;
    entry.nextFree = kNoNode;
    freeRanges--;
}

void RangeAllocator::retireNode(uint32_t node){
    Node& entry = nodes[node];
    if (entry.previousPhysical != kNoNode)
        nodes[entry.previousPhysical].nextPhysical = entry.nextPhysical;
    if (entry.nextPhysical != kNoNode)
        nodes[entry.nextPhysical].previousPhysical = entry.previousPhysical;
    entry = Node();
    entry.nextFree = unusedHead;
    unusedHead = node;
}

uint32_t RangeAllocator::takeNode(){
    if (unusedHead != kNoNode){
        const uint32_t node = unusedHead;
        unusedHead = nodes[node].nextFree;
        nodes[node].nextFree = kNoNode;
        return node;
    }
    assert(nodes.size() < nodes.capacity() && "allocate provisions a record before it splits");
    nodes.emplace_back();
    return (uint32_t)(nodes.size() - 1);
}

RangeAllocator::Allocation RangeAllocator::allocate(uint64_t units){
    if (units == 0 || units > capacity)
        return {};
    const uint32_t node = findFit(units);
    if (node == kNoNode)
        return {};

    // provision before anything changes: a split may need one record, and every outstanding
    // range may later wait in the deferred queue at once
    try{
        if (unusedHead == kNoNode)
            reserveGeometric(nodes, nodes.size() + 1);
        reserveGeometric(pending, (size_t)outstanding + 1);
    }catch(const std::bad_alloc&){
        return {};
    }

    removeFree(node);
    if (nodes[node].size > units){
        const uint32_t remainder = takeNode();
        Node& taken = nodes[node];
        Node& rest = nodes[remainder];
        rest.offset = taken.offset + units;
        rest.size = taken.size - units;
        rest.previousPhysical = node;
        rest.nextPhysical = taken.nextPhysical;
        if (taken.nextPhysical != kNoNode)
            nodes[taken.nextPhysical].previousPhysical = remainder;
        taken.nextPhysical = remainder;
        taken.size = units;
        insertFree(remainder);
    }

    Node& taken = nodes[node];
    taken.state = NodeState::Live;
    used += units;
    peak = std::max(peak, used);
    end = std::max(end, taken.offset + units);
    outstanding++;
    publishStats();
    Allocation allocation;
    allocation.offset = taken.offset;
    allocation.size = units;
    allocation.node = node;
    return allocation;
}

bool RangeAllocator::isLiveAllocation(const Allocation& allocation) const{
    return allocation.node < nodes.size() && nodes[allocation.node].state == NodeState::Live
        && nodes[allocation.node].offset == allocation.offset && nodes[allocation.node].size == allocation.size;
}

void RangeAllocator::releaseNode(uint32_t node) noexcept{
    used -= nodes[node].size;
    outstanding--;

    const uint32_t previous = nodes[node].previousPhysical;
    if (previous != kNoNode && nodes[previous].state == NodeState::Free){
        removeFree(previous);
        nodes[previous].size += nodes[node].size;
        retireNode(node);
        node = previous;
    }
    const uint32_t next = nodes[node].nextPhysical;
    if (next != kNoNode && nodes[next].state == NodeState::Free){
        removeFree(next);
        nodes[node].size += nodes[next].size;
        retireNode(next);
    }
    insertFree(node);
}

void RangeAllocator::free(Allocation allocation) noexcept{
    if (!allocation.isValid())
        return;
    assert(isLiveAllocation(allocation) && "this range is not live in this RangeAllocator: freed twice, "
                                           "queued by freeAfter, or from another allocator");
    releaseNode(allocation.node);
    publishStats();
}

void RangeAllocator::freeAfter(Allocation allocation, uint64_t retireStamp) noexcept{
    if (!allocation.isValid())
        return;
    assert(isLiveAllocation(allocation) && "this range is not live in this RangeAllocator: freed twice, "
                                           "queued by freeAfter, or from another allocator");
    assert(pending.size() < pending.capacity() && "allocate provisions a queue entry per outstanding range");
    nodes[allocation.node].state = NodeState::Pending;
    pending.push_back({retireStamp, allocation.node});
    earliestPendingStamp = std::min(earliestPendingStamp, retireStamp);
}

void RangeAllocator::reclaimThrough(uint64_t safeStamp) noexcept{
    if (safeStamp < earliestPendingStamp)
        return;
    uint64_t earliest = UINT64_MAX;
    size_t kept = 0;
    for (const PendingRange& entry : pending){
        if (entry.retireStamp <= safeStamp){
            releaseNode(entry.node);
            continue;
        }
        earliest = std::min(earliest, entry.retireStamp);
        pending[kept++] = entry;
    }
    pending.resize(kept);
    earliestPendingStamp = earliest;
    publishStats();
}

bool RangeAllocator::tryGrowInPlace(Allocation& allocation, uint64_t units){
    assert(isLiveAllocation(allocation) && "tryGrowInPlace needs a live range of this RangeAllocator");
    assert(units > allocation.size && "tryGrowInPlace grows: the range already holds this many units");
    const uint32_t next = nodes[allocation.node].nextPhysical;
    if (next == kNoNode || nodes[next].state != NodeState::Free || nodes[next].size < units - allocation.size)
        return false;

    const uint64_t delta = units - allocation.size;
    removeFree(next);
    if (nodes[next].size == delta){
        retireNode(next);
    }else{
        nodes[next].offset += delta;
        nodes[next].size -= delta;
        insertFree(next);
    }
    nodes[allocation.node].size = units;
    allocation.size = units;
    used += delta;
    peak = std::max(peak, used);
    end = std::max(end, allocation.offset + units);
    publishStats();
    return true;
}

RangeAllocator::Stats RangeAllocator::getStats() const{
    Stats stats;
    stats.capacityUnits = capacity;
    stats.usedUnits = used;
    stats.peakUnits = peak;
    stats.endUnits = end;
    stats.freeRanges = freeRanges;
    stats.pendingRanges = (uint32_t)pending.size();
    if (firstLevelMap != 0){
        const uint32_t firstLevel = highestBit(firstLevelMap);
        const uint32_t secondLevel = highestBit((uint64_t)secondLevelMaps[firstLevel]);
        for (uint32_t node = freeHeads[firstLevel * kSecondLevelCount + secondLevel]; node != kNoNode; node = nodes[node].nextFree)
            stats.largestFreeUnits = std::max(stats.largestFreeUnits, nodes[node].size);
    }
    const uint64_t freeUnits = capacity - used;
    if (freeUnits != 0)
        stats.fragmentation = (float)(freeUnits - stats.largestFreeUnits) / (float)freeUnits;
    return stats;
}

void RangeAllocator::publishStats() noexcept{
    detail::publish(record, used * unitBytes, peak * unitBytes, capacity * unitBytes, outstanding);
}
