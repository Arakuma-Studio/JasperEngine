// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/Arena.h"

#include <algorithm>
#include <cstdlib>

using namespace jasperengine::htmlui::compat;

namespace {

    thread_local Arena* tCurrent = nullptr;

    Arena& threadArena(){
        static thread_local Arena arena;
        return arena;
    }

    // Every regular block has this size, and a thread keeps the blocks its arenas release
    // for its next arenas (up to 4 MB): a rewriter session per document, per style sheet
    // reload and per inline style would otherwise map and unmap the same memory each time
    // (the page faults cost a quarter of a 50 KB style sheet's rewrite).
    constexpr size_t kBlockSize = 256 * 1024;
    constexpr size_t kKeptBlocks = kArenaKeptBytes / kBlockSize;
    static_assert(kKeptBlocks * kBlockSize == kArenaKeptBytes, "whole blocks");

    thread_local size_t tSpareBytes = 0;   // SpareStorage

    struct BlockCache {
        void* blocks[kKeptBlocks];
        size_t count = 0;

        ~BlockCache(){
            while (count > 0)
                std::free(blocks[--count]);
        }
    };
    thread_local BlockCache tBlocks;

}

bool SpareStorage::keep(size_t previous, size_t bytes){
    const size_t others = tSpareBytes >= previous ? tSpareBytes - previous : 0;
    if (others + bytes > kSpareKeptBytes)
        return false;
    tSpareBytes = others + bytes;
    return true;
}

void SpareStorage::take(size_t bytes){
    tSpareBytes = tSpareBytes >= bytes ? tSpareBytes - bytes : 0;
}

size_t SpareStorage::kept(){
    return tSpareBytes;
}

Arena& Arena::current(){
    Arena* arena = tCurrent;
    return arena ? *arena : threadArena();
}

void* Arena::grow(size_t bytes){
    const size_t header = (sizeof(Block) + 7u) & ~(size_t)7u;
    Block* block = nullptr;
    size_t size = kBlockSize;
    if (bytes + header <= kBlockSize){
        BlockCache& cache = tBlocks;
        block = (Block*)(cache.count > 0 ? cache.blocks[--cache.count] : std::malloc(kBlockSize));
    }else{
        // (larger than a block: its own allocation, returned to the heap)
        size = bytes + header;
        block = (Block*)std::malloc(size);
    }
    block->next = blocks;
    block->size = size;
    blocks = block;
    total += size;
    char* at = (char*)block + header;
    cursor = at + bytes;
    end = (char*)block + size;
    return at;
}

void Arena::release(){
    BlockCache& cache = tBlocks;
    while (blocks){
        Block* next = blocks->next;
        if (blocks->size == kBlockSize && cache.count < kKeptBlocks)
            cache.blocks[cache.count++] = blocks;
        else
            std::free(blocks);
        blocks = next;
    }
    cursor = end = nullptr;
    total = 0;
}

ArenaScope::ArenaScope(Arena& arena): previous(tCurrent) {
    tCurrent = &arena;
}

ArenaScope::~ArenaScope(){
    tCurrent = previous;
}
