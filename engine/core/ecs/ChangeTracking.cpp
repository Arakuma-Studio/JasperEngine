// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ChangeTracking.h"

// The ECS page arena (PagedStorage.h) lives here too: one definition for the engine library
// and every module that instantiates the component arrays.
// (not under ASan: its heap redzones catch an overrun past a page)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define JE_ECS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define JE_ECS_ASAN 1
#endif
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(JE_ECS_ASAN)
#define JE_ECS_PAGE_ARENA 1
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#endif

using namespace jasperengine;

namespace {

#if JE_ECS_PAGE_ARENA
    constexpr size_t kChunkBytes = size_t(2) << 20;
    constexpr size_t kMaxArenaPage = size_t(512) * 1024;

    // 2 MiB of pages of one size: carved in order, freed ones reused first; the chunk goes back
    // to the system when its last page is freed
    struct Chunk {
        char* base = nullptr;
        size_t pageBytes = 0;
        size_t capacity = 0;
        size_t carved = 0;
        size_t live = 0;
        std::vector<void*> freePages;

        bool full() const { return freePages.empty() && carved == capacity; }
    };

    // Empty chunks kept for reuse (a scene loaded again, an array that grows back): up to
    // this many, their memory left to the kernel to reclaim under pressure (MADV_FREE)
    constexpr size_t kMaxIdleChunks = 64;

    struct PageArena {
        std::mutex mutex;
        std::unordered_map<uintptr_t, Chunk*> chunks;         // by base address
        std::unordered_map<size_t, std::vector<Chunk*>> open; // page size -> chunks with room
        std::vector<Chunk*> idle;                             // empty, any page size
    };

    // never destroyed: arrays of static registries free their pages during static destruction
    PageArena& pageArena(){
        static PageArena* arena = new PageArena();
        return *arena;
    }

    Chunk* mapChunk(size_t pageBytes){
        // twice the size, then the unaligned ends unmapped: a 2 MiB-aligned 2 MiB range
        void* raw = mmap(nullptr, kChunkBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED){
            return nullptr;
        }
        const uintptr_t start = (uintptr_t)raw;
        const uintptr_t aligned = (start + kChunkBytes - 1) & ~(uintptr_t)(kChunkBytes - 1);
        if (aligned > start){
            munmap(raw, aligned - start);
        }
        const uintptr_t end = start + kChunkBytes * 2;
        if (end > aligned + kChunkBytes){
            munmap((void*)(aligned + kChunkBytes), end - (aligned + kChunkBytes));
        }
        // a hint: without transparent huge pages it is a plain mapping
        madvise((void*)aligned, kChunkBytes, MADV_HUGEPAGE);

        Chunk* chunk = new Chunk();
        chunk->base = (char*)aligned;
        chunk->pageBytes = pageBytes;
        chunk->capacity = kChunkBytes / pageBytes;
        return chunk;
    }
#endif

    // the registry and tick of the scheduled system running on this thread
    struct ThreadSystemTick {
        const ChangeClock* clock = nullptr;
        Tick tick = 0;
    };

    thread_local ThreadSystemTick threadSystemTick;

}

Tick ChangeClock::stamp(){
    if (threadSystemTick.clock == this){
        return threadSystemTick.tick;
    }
    if (sampled){
        current++;
        sampled = false;
    }
    return current;
}

void ChangeClock::setThreadSystemTick(const ChangeClock* clock, Tick tick){
    threadSystemTick.clock = clock;
    threadSystemTick.tick = tick;
}

void ChangeClock::clearThreadSystemTick(){
    threadSystemTick.clock = nullptr;
    threadSystemTick.tick = 0;
}

void* ecs::allocateArenaPage(size_t bytes){
#if JE_ECS_PAGE_ARENA
    if (bytes == 0 || bytes > kMaxArenaPage){
        return nullptr;
    }
    // 4 KiB-aligned pages
    bytes = (bytes + 4095) & ~size_t(4095);

    PageArena& arena = pageArena();
    std::lock_guard<std::mutex> lock(arena.mutex);
    std::vector<Chunk*>& open = arena.open[bytes];
    if (open.empty()){
        Chunk* chunk;
        if (!arena.idle.empty()){
            chunk = arena.idle.back();
            arena.idle.pop_back();
            chunk->pageBytes = bytes;
            chunk->capacity = kChunkBytes / bytes;
        }else{
            chunk = mapChunk(bytes);
            if (!chunk){
                return nullptr;
            }
            arena.chunks[(uintptr_t)chunk->base] = chunk;
        }
        // its free list allocated now: freeing pages (compaction during a mass destroy) then
        // allocates nothing (a malloc there consolidates the allocator's freed small blocks)
        chunk->freePages.reserve(chunk->capacity);
        open.push_back(chunk);
    }

    Chunk* chunk = open.back();
    void* page;
    if (!chunk->freePages.empty()){
        page = chunk->freePages.back();
        chunk->freePages.pop_back();
    }else{
        page = chunk->base + chunk->carved * bytes;
        chunk->carved++;
    }
    chunk->live++;
    if (chunk->full()){
        open.pop_back();
    }
    return page;
#else
    (void)bytes;
    return nullptr;
#endif
}

bool ecs::freeArenaPage(void* page, size_t bytes){
#if JE_ECS_PAGE_ARENA
    if (!page || bytes == 0 || bytes > kMaxArenaPage){
        return false;
    }
    bytes = (bytes + 4095) & ~size_t(4095);

    PageArena& arena = pageArena();
    std::lock_guard<std::mutex> lock(arena.mutex);
    auto it = arena.chunks.find((uintptr_t)page & ~(uintptr_t)(kChunkBytes - 1));
    if (it == arena.chunks.end()){
        return false;
    }

    Chunk* chunk = it->second;
    const bool wasFull = chunk->full();
    chunk->live--;
    if (chunk->live == 0){
        if (!wasFull){
            std::vector<Chunk*>& open = arena.open[bytes];
            open.erase(std::remove(open.begin(), open.end(), chunk), open.end());
        }
        chunk->carved = 0;
        chunk->freePages.clear();
        if (arena.idle.size() < kMaxIdleChunks){
#ifdef MADV_FREE
            madvise(chunk->base, kChunkBytes, MADV_FREE);
#endif
            arena.idle.push_back(chunk);
        }else{
            munmap(chunk->base, kChunkBytes);
            arena.chunks.erase(it);
            delete chunk;
        }
        return true;
    }
    chunk->freePages.push_back(page);
    if (wasFull){
        arena.open[bytes].push_back(chunk);
    }
    return true;
#else
    (void)page;
    (void)bytes;
    return false;
#endif
}
