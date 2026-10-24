// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::ObjectPool (Modules/Memory/Source/ObjectPool.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// Block acquisition, the checked builds' bit writes and checks, and the pool's destruction.
// take and giveBack are inline in the header; everything here runs once per block, only in
// checked builds, or only while the pool is destroyed.

#include "ObjectPool.h"

#include "memory/Arena.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace jasperengine;
using namespace jasperengine::memory;
using namespace jasperengine::memory::detail;

namespace{
    constexpr size_t kFirstBlockMinimumSlots = 16;
    constexpr size_t kFirstBlockSlotBytes = 16 * 1024;
    constexpr size_t kBitsPerWord = 64;
    constexpr size_t kCacheLine = 64;

    size_t roundUp(size_t value, size_t multiple){
        return (value + multiple - 1) / multiple * multiple;
    }

    size_t bitmapBytes(size_t slotCount){
        return roundUp(slotCount, kBitsPerWord) / 8;
    }

    size_t bitmapWords(size_t slotCount){
        return roundUp(slotCount, kBitsPerWord) / kBitsPerWord;
    }

    bool testBit(const uint64_t* bitmap, size_t slot){
        return (bitmap[slot / kBitsPerWord] >> (slot % kBitsPerWord)) & 1u;
    }

    void setBit(uint64_t* bitmap, size_t slot){
        bitmap[slot / kBitsPerWord] |= uint64_t(1) << (slot % kBitsPerWord);
    }

    void clearBit(uint64_t* bitmap, size_t slot){
        bitmap[slot / kBitsPerWord] &= ~(uint64_t(1) << (slot % kBitsPerWord));
    }
}

ObjectPoolStorage::ObjectPoolStorage(std::string_view statsName, size_t objectBytes, size_t objectAlignment, bool assertNoLeaks):
    slotBytes(roundUp(std::max(objectBytes, sizeof(void*)), std::max(objectAlignment, alignof(void*)))),
    blockAlignment(std::max(objectAlignment, kCacheLine)),
    assertNoLeaks(assertNoLeaks),
    record(registerRecord(statsName.empty() ? std::string_view("ObjectPool") : statsName, StatsKind::Pool, Residency::Cpu)){
    assert(!statsName.empty() && "an ObjectPool needs a stats name; name it after its owner, as 'Area.What'");
}

ObjectPoolStorage::~ObjectPoolStorage(){
    unregisterRecord(record);
    for (const BlockDescriptor& block : blocks){
        const size_t blockBytes = block.slotCount * slotBytes + bitmapBytes(block.slotCount);
        JE_MEMORY_UNPOISON(block.begin, blockBytes);
        ::operator delete(block.begin, std::align_val_t(blockAlignment));
        (void)blockBytes;
    }
}

void* ObjectPoolStorage::takeFromNewBlock(){
    const size_t capSlots = std::max<size_t>(1, kArenaMaxBlockBytes / slotBytes);
    const size_t growthSlots = blocks.empty()
        ? std::max(kFirstBlockMinimumSlots, kFirstBlockSlotBytes / slotBytes)
        : newestSlotCount * 2;
    const size_t slotCount = std::min(growthSlots, capSlots);
    const size_t slotsBytes = slotCount * slotBytes;
    const size_t blockBytes = slotsBytes + bitmapBytes(slotCount);

    // the descriptor's place first, so a failure to grow the vector cannot leak the block
    if (blocks.size() == blocks.capacity())
        blocks.reserve(std::max<size_t>(8, blocks.capacity() * 2));
    auto* begin = static_cast<unsigned char*>(::operator new(blockBytes, std::align_val_t(blockAlignment)));
    auto* bitmap = reinterpret_cast<uint64_t*>(begin + slotsBytes);
    std::fill_n(bitmap, bitmapWords(slotCount), uint64_t(0));
    // the slots stay poisoned until the bump hands them out; the bitmap stays readable
    JE_MEMORY_POISON(begin, slotsBytes);
    const BlockDescriptor descriptor{begin, (uint32_t)slotCount, bitmap};
    const auto place = std::upper_bound(blocks.begin(), blocks.end(), begin,
        [](const unsigned char* address, const BlockDescriptor& block){ return address < block.begin; });
    blocks.insert(place, descriptor);

    newestBegin = begin;
    newestSlotCount = slotCount;
    cursor = begin + slotBytes;
    limit = begin + slotsBytes;
    reservedBytes += blockBytes;
    // counts the slot being taken
    liveCount++;
    peakLive = std::max(peakLive, liveCount);
    publishStats();
    liveCount--;
    return begin;
}

size_t ObjectPoolStorage::usedSlots(const BlockDescriptor& block) const{
    if (block.begin != newestBegin)
        return block.slotCount;
    return (size_t)(cursor - block.begin) / slotBytes;
}

ObjectPoolStorage::SlotLocation ObjectPoolStorage::locate(const void* slot) const{
    const auto* address = static_cast<const unsigned char*>(slot);
    const auto after = std::upper_bound(blocks.begin(), blocks.end(), address,
        [](const unsigned char* value, const BlockDescriptor& block){ return value < block.begin; });
    if (after == blocks.begin()){
        std::fprintf(stderr, "ObjectPool: a pointer that did not come from this pool\n");
        assert(false && "this pointer did not come from this pool");
        std::abort();
    }
    const BlockDescriptor& block = *(after - 1);
    const size_t offset = (size_t)(address - block.begin);
    if (offset >= block.slotCount * slotBytes || offset % slotBytes != 0){
        std::fprintf(stderr, "ObjectPool: a pointer that did not come from this pool\n");
        assert(false && "this pointer did not come from this pool");
        std::abort();
    }
    return {(size_t)(after - 1 - blocks.begin()), offset / slotBytes};
}

void ObjectPoolStorage::markLive(void* slot){
    const SlotLocation location = locate(slot);
    setBit(blocks[location.block].bitmap, location.slot);
}

void ObjectPoolStorage::checkLiveAndMarkFree(void* slot){
#if JE_MEMORY_CHECKS
    SerialUseScope use(serialUse, "ObjectPool");
    const SlotLocation location = locate(slot);
    uint64_t* bitmap = blocks[location.block].bitmap;
    if (!testBit(bitmap, location.slot)){
        std::fprintf(stderr, "ObjectPool: an object destroyed twice\n");
        assert(false && "this object was already returned to the pool");
    }
    clearBit(bitmap, location.slot);
#else
    (void)slot;
#endif
}

bool ObjectPoolStorage::claimWhileDestroying(void* slot){
    SerialUseScope use(serialUse, "ObjectPool");
    const SlotLocation location = locate(slot);
    uint64_t* bitmap = blocks[location.block].bitmap;
    if (!testBit(bitmap, location.slot))
        return false;
    clearBit(bitmap, location.slot);
    liveCount--;
    return true;
}

void ObjectPoolStorage::retireWhileDestroying(void* slot){
#if JE_MEMORY_CHECKS
    std::memset(slot, 0xDD, slotBytes);
#endif
    JE_MEMORY_POISON(slot, slotBytes);
    (void)slot;
}

void ObjectPoolStorage::deriveLiveSet(){
    size_t used = 0;
    for (const BlockDescriptor& block : blocks){
        const size_t usedInBlock = usedSlots(block);
        used += usedInBlock;
        const size_t fullWords = usedInBlock / kBitsPerWord;
        std::fill_n(block.bitmap, fullWords, ~uint64_t(0));
        std::fill(block.bitmap + fullWords, block.bitmap + bitmapWords(block.slotCount), uint64_t(0));
        if (usedInBlock % kBitsPerWord != 0)
            block.bitmap[fullWords] = (uint64_t(1) << (usedInBlock % kBitsPerWord)) - 1;
    }
    // a freelist holds at most the used slots: without per-slot bits (unchecked builds) a
    // double destroy links one slot twice and the freelist becomes a cycle
    size_t links = 0;
    for (void* slot = freeHead; slot; ){
        if (++links > used){
            std::fprintf(stderr, "ObjectPool: a slot was destroyed twice; the freelist is a cycle\n");
            std::abort();
        }
        const SlotLocation location = locate(slot);
        clearBit(blocks[location.block].bitmap, location.slot);
        JE_MEMORY_UNPOISON(slot, sizeof(void*));
        std::memcpy(&slot, slot, sizeof(void*));
    }
}

void ObjectPoolStorage::destroyLiveObjects(void (*destroy)(void*)) noexcept{
#if JE_MEMORY_CHECKS
    if (liveCount != 0 && assertNoLeaks){
        std::fprintf(stderr, "ObjectPool: %zu object(s) still alive when the pool is destroyed (leak)\n", liveCount);
        assert(liveCount == 0 && "objects of this ObjectPool were never destroyed");
    }
#endif
    deriveLiveSet();
    destroying = true;
    for (size_t blockIndex = 0; blockIndex < blocks.size(); blockIndex++){
        for (size_t slotIndex = 0; slotIndex < usedSlots(blocks[blockIndex]); slotIndex++){
            const BlockDescriptor& block = blocks[blockIndex];
            if (!testBit(block.bitmap, slotIndex))
                continue;
            // cleared first: a destroy of this object from a sibling's destructor is a no-op
            clearBit(block.bitmap, slotIndex);
            liveCount--;
            void* slot = block.begin + slotIndex * slotBytes;
            destroy(slot);
            retireWhileDestroying(slot);
        }
    }
}

void ObjectPoolStorage::publishStats(){
    publish(record, (uint64_t)liveCount * slotBytes, (uint64_t)peakLive * slotBytes, reservedBytes, liveCount);
}
