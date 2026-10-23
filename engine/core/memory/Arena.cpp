// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::Arena (Modules/Memory/Source/Arena.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// The out-of-line paths: block acquisition, destructor records, rewind and reset. The bump
// itself is inline in the header; everything here runs at most once per block, per recorded
// destructor, or per rewind and reset.

#include "Arena.h"

#include <cassert>
#include <limits>
#include <string>

using namespace jasperengine;
using namespace jasperengine::memory;

namespace{
    // Blocks are cache-line aligned, so every alignment up to it costs no padding at a
    // block's start and a block never shares a line with another allocation.
    constexpr size_t kBlockAlignment = 64;

    unsigned char* acquireBlockMemory(size_t bytes){
        auto* data = static_cast<unsigned char*>(::operator new(bytes, std::align_val_t(kBlockAlignment)));
        JE_MEMORY_POISON(data, bytes);
        return data;
    }

    void releaseBlockMemory(unsigned char* data, size_t bytes){
        JE_MEMORY_UNPOISON(data, bytes);
        (void)bytes;
        ::operator delete(data, std::align_val_t(kBlockAlignment));
    }

    size_t roundUpToBlockAlignment(size_t bytes){
        return (bytes + (kBlockAlignment - 1)) & ~(kBlockAlignment - 1);
    }

    // A block serving `bytes` at `alignment` at any address: worst-case padding plus the request.
    size_t blockBytesFor(size_t bytes, size_t alignment){
        const size_t padding = alignment > kBlockAlignment ? alignment : 0;
        if (bytes > std::numeric_limits<size_t>::max() - padding - kBlockAlignment)
            throw std::bad_alloc();
        return roundUpToBlockAlignment(bytes + padding);
    }
}

Arena::Arena(std::string_view statsName, size_t firstBlock):
    firstBlockBytes(roundUpToBlockAlignment(firstBlock != 0 ? firstBlock : kArenaFirstBlockBytes)),
    record(detail::registerRecord(statsName.empty() ? std::string_view("Arena") : statsName, StatsKind::Arena, Residency::Cpu)){
    assert(!statsName.empty() && "an Arena needs a stats name; name it after its owner, as 'Area.What'");
}

Arena::~Arena(){
    runDestructorsAbove(0);
    detail::unregisterRecord(record);
    for (const Block& block : blocks)
        releaseBlockMemory(block.data, block.capacity);
}

void Arena::retireRange(unsigned char* begin, size_t bytes){
    JE_MEMORY_UNPOISON(begin, bytes);
#if JE_MEMORY_CHECKS
    std::memset(begin, 0xDD, bytes);
#endif
    JE_MEMORY_POISON(begin, bytes);
    (void)begin;
    (void)bytes;
}

void* Arena::allocateFromNextBlock(size_t bytes, size_t alignment){
    if (!blocks.empty()){
        Block& currentBlock = blocks[current];
        currentBlock.used = cursor - reinterpret_cast<uintptr_t>(currentBlock.data);
        peakSinceReset = std::max(peakSinceReset, usedBeforeCurrent + currentBlock.used);
        // blocks after the current one are empty: a rewind empties every block it passes
        for (size_t next = current + 1; next < blocks.size(); next++){
            const uintptr_t data = reinterpret_cast<uintptr_t>(blocks[next].data);
            const uintptr_t end = data + blocks[next].capacity;
            const uintptr_t aligned = (data + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
            if (aligned > end || bytes > end - aligned)
                continue;
            for (size_t skipped = current; skipped < next; skipped++)
                usedBeforeCurrent += blocks[skipped].used;
            current = next;
            cursor = data;
            limit = end;
            return tryBump(bytes, alignment);
        }
    }

    const size_t growthStep = blocks.empty() ? firstBlockBytes : std::min(blocks.back().capacity * 2, kArenaMaxBlockBytes);
    const size_t blockBytes = std::max(growthStep, blockBytesFor(bytes, alignment));
    if (blocks.size() == blocks.capacity())
        blocks.reserve(std::max<size_t>(4, blocks.capacity() * 2));
    unsigned char* data = acquireBlockMemory(blockBytes);
    for (size_t passed = current; passed < blocks.size(); passed++)
        usedBeforeCurrent += blocks[passed].used;
    blocks.push_back({data, blockBytes, 0});
    current = blocks.size() - 1;
    cursor = reinterpret_cast<uintptr_t>(data);
    limit = cursor + blockBytes;
    void* memory = tryBump(bytes, alignment);
    publishStats();
    return memory;
}

void* Arena::allocateArrayBytes(size_t count, size_t elementBytes, size_t alignment){
    if (elementBytes != 0 && count > std::numeric_limits<size_t>::max() / elementBytes)
        throw std::bad_array_new_length();
    return allocate(count * elementBytes, alignment);
}

void Arena::reserveDestructorRecord(){
    detail::SerialUseScope use(serialUse, "Arena");
    if (destructors.size() == destructors.capacity())
        destructors.reserve(std::max<size_t>(64, destructors.capacity() * 2));
}

void Arena::recordDestructor(void* object, void (*destroy)(void*)){
    bool recorded = false;
    {
        detail::SerialUseScope use(serialUse, "Arena");
        // the slot reserved before construction may have been taken by an allocate<U>
        // inside the constructor, so this push can still grow the vector
        try{
            destructors.push_back({object, destroy});
            recorded = true;
        }catch(const std::bad_alloc&){
        }
    }
    if (!recorded){
        // destroyed rather than leaked, outside the hold: the destructor may use the arena
        destroy(object);
        throw std::bad_alloc();
    }
}

void Arena::runDestructorsAbove(uint32_t count){
    // not held across the destructor itself, which may allocate from this arena
    for (;;){
        DestructorRecord entry;
        {
            detail::SerialUseScope use(serialUse, "Arena");
            if (destructors.size() <= count)
                return;
            entry = destructors.back();
            destructors.pop_back();
        }
        entry.destroy(entry.object);
    }
}

Arena::Marker Arena::marker() const{
    Marker result;
    result.block = (uint32_t)current;
    result.destructorCount = (uint32_t)destructors.size();
    result.offset = blocks.empty() ? 0 : cursor - reinterpret_cast<uintptr_t>(blocks[current].data);
    return result;
}

void Arena::rewind(Marker target){
    assert(target.destructorCount <= destructors.size() && "rewind to a marker taken before a later rewind or reset");
    runDestructorsAbove(target.destructorCount);

    detail::SerialUseScope use(serialUse, "Arena");
    if (!blocks.empty()){
        assert(target.block <= current && "rewind to a marker taken before a later rewind or reset");
        Block& currentBlock = blocks[current];
        currentBlock.used = cursor - reinterpret_cast<uintptr_t>(currentBlock.data);
        peakSinceReset = std::max(peakSinceReset, usedBeforeCurrent + currentBlock.used);
        for (size_t block = target.block + 1; block <= current; block++){
            retireRange(blocks[block].data, blocks[block].used);
            blocks[block].used = 0;
        }
        Block& targetBlock = blocks[target.block];
        assert(target.offset <= targetBlock.used && "rewind to a marker taken before a later rewind or reset");
        retireRange(targetBlock.data + target.offset, targetBlock.used - target.offset);
        targetBlock.used = target.offset;
        usedBeforeCurrent = 0;
        for (size_t block = 0; block < target.block; block++)
            usedBeforeCurrent += blocks[block].used;
        current = target.block;
        cursor = reinterpret_cast<uintptr_t>(targetBlock.data) + target.offset;
        limit = reinterpret_cast<uintptr_t>(targetBlock.data) + targetBlock.capacity;
    }
    publishStats();
}

void Arena::reset(){
    runDestructorsAbove(0);

    detail::SerialUseScope use(serialUse, "Arena");
    if (!blocks.empty()){
        Block& currentBlock = blocks[current];
        currentBlock.used = cursor - reinterpret_cast<uintptr_t>(currentBlock.data);
        peakSinceReset = std::max(peakSinceReset, usedBeforeCurrent + currentBlock.used);
        if (blocks.size() > 1){
            // one block of the high-water size, acquired before the chain is freed so a failed
            // acquisition leaves the arena whole
            const size_t blockBytes = roundUpToBlockAlignment(std::max(peakSinceReset, firstBlockBytes));
            unsigned char* data = acquireBlockMemory(blockBytes);
            for (const Block& block : blocks)
                releaseBlockMemory(block.data, block.capacity);
            blocks.clear();
            blocks.push_back({data, blockBytes, 0});
        }else{
            retireRange(currentBlock.data, currentBlock.used);
            currentBlock.used = 0;
        }
        current = 0;
        cursor = reinterpret_cast<uintptr_t>(blocks[0].data);
        limit = cursor + blocks[0].capacity;
    }
    usedBeforeCurrent = 0;
    // published before the peak is cleared, so the high water takes this reset's peak
    publishStats();
    peakSinceReset = 0;
}

size_t Arena::bytesUsed() const{
    if (blocks.empty())
        return 0;
    return usedBeforeCurrent + (cursor - reinterpret_cast<uintptr_t>(blocks[current].data));
}

size_t Arena::bytesReserved() const{
    size_t reserved = 0;
    for (const Block& block : blocks)
        reserved += block.capacity;
    return reserved;
}

void Arena::publishStats(){
    const size_t used = bytesUsed();
    highWater = std::max(highWater, std::max(used, peakSinceReset));
    detail::publish(record, used, highWater, bytesReserved(), blocks.size());
}

// ---- FrameArena

FrameArena::FrameArena(std::string_view statsName){
    for (int i = 0; i < kFramesInFlight; i++){
        std::string name(statsName);
        name += "." + std::to_string(i);
        arenas[i] = std::make_unique<Arena>(name);
    }
}

void FrameArena::beginFrame(){
    index = (index + 1) % kFramesInFlight;
    arenas[index]->reset();
}

FrameArena& memory::frameArena(){
    static FrameArena* arena = new FrameArena("Engine.FrameArena");
    return *arena;
}
