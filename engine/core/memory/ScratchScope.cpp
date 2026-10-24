// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::ScratchScope (Modules/Memory/Source/ScratchScope.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "ScratchScope.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::memory;

namespace{
    // outermost closes per retention window
    constexpr uint32_t kTrimWindow = 1024;

    std::atomic<uint32_t> nextScratchThread{1};

    struct ScratchThread {
        std::unique_ptr<Arena> arena;
        uint32_t openDepth = 0;
        uint64_t outOfOrder = 0;
        // the largest need of an outermost scope in this window and the last one
        size_t windowPeak = 0;
        size_t lastWindowPeak = 0;
        uint32_t windowCloses = 0;

        Arena& getArena(){
            if (!arena){
                char name[48];
                std::snprintf(name, sizeof(name), "Scratch.thread-%u", nextScratchThread.fetch_add(1, std::memory_order_relaxed));
                arena = std::make_unique<Arena>(name);
            }
            return *arena;
        }

        ~ScratchThread();
    };

    // trivially destructible, so it reads correctly after the thread's state is destroyed
    thread_local bool scratchGone = false;
    thread_local ScratchThread scratchThread;

    ScratchThread::~ScratchThread(){
        scratchGone = true;
    }

    // a scope opened after the thread's scratch state has gone counts its depth here, never
    // equal to depth + 1 for any scope, so all of its allocations go aside
    const uint32_t kNeverInnermost = 0;
}

struct ScratchScope::AsideBlock {
    AsideBlock* next;
    size_t alignment;
};

ScratchScope::ScratchScope(): aside(nullptr){
    if (scratchGone){
        arena = nullptr;
        openDepth = &kNeverInnermost;
        depth = UINT32_MAX;
        return;
    }
    ScratchThread& thread = scratchThread;
    arena = &thread.getArena();
    openDepth = &thread.openDepth;
    depth = thread.openDepth++;
    mark = arena->marker();
}

ScratchScope::~ScratchScope(){
    while (aside){
        AsideBlock* next = aside->next;
        const size_t alignment = aside->alignment;
        ::operator delete(static_cast<void*>(aside), std::align_val_t(alignment));
        aside = next;
    }
    if (!arena)
        return;
    ScratchThread& thread = scratchThread;
    assert(thread.openDepth == depth + 1 && "a ScratchScope closes as the innermost open scope (declare scopes as locals)");
    thread.openDepth = depth;
    if (depth > 0){
        arena->rewind(mark);
        return;
    }
    // outermost: keep what the outermost scopes need, release a spike after a window
    const size_t used = arena->bytesUsed();
    thread.windowPeak = std::max(thread.windowPeak, used);
    if (++thread.windowCloses >= kTrimWindow){
        const size_t need = std::max(thread.windowPeak, thread.lastWindowPeak);
        thread.lastWindowPeak = thread.windowPeak;
        thread.windowPeak = 0;
        thread.windowCloses = 0;
        if (arena->bytesReserved() > 2 * std::max(need, kArenaFirstBlockBytes)){
            // a fresh arena: its first block is the default, and the chain the steady need
            // grows coalesces at the next reset
            char name[48];
            std::snprintf(name, sizeof(name), "Scratch.thread-%u", nextScratchThread.fetch_add(1, std::memory_order_relaxed));
            thread.arena = std::make_unique<Arena>(name);
            return;
        }
    }
    arena->reset();
}

void* ScratchScope::allocateAside(size_t bytes, size_t alignment){
    if (!scratchGone)
        scratchThread.outOfOrder++;
    const size_t align = std::max(alignment, alignof(AsideBlock));
    const size_t header = (sizeof(AsideBlock) + align - 1) / align * align;
    auto* memory = static_cast<unsigned char*>(::operator new(header + bytes, std::align_val_t(align)));
    auto* block = reinterpret_cast<AsideBlock*>(memory);
    block->next = aside;
    block->alignment = align;
    aside = block;
    return memory + header;
}

uint64_t ScratchScope::outOfOrderCount(){
    return scratchGone ? 0 : scratchThread.outOfOrder;
}
