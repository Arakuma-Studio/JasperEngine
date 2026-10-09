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

    constexpr size_t kFirstBlock = 16 * 1024;
    constexpr size_t kLargestBlock = 1024 * 1024;

}

Arena& Arena::current(){
    Arena* arena = tCurrent;
    return arena ? *arena : threadArena();
}

void* Arena::grow(size_t bytes){
    // blocks double up to 1 MB (a 50 KB style sheet takes about 1 MB in all)
    size_t size = blocks ? std::min(blocks->size * 2, kLargestBlock) : kFirstBlock;
    const size_t header = (sizeof(Block) + 7u) & ~(size_t)7u;
    if (size < bytes + header)
        size = bytes + header;
    Block* block = (Block*)std::malloc(size);
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
    while (blocks){
        Block* next = blocks->next;
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
