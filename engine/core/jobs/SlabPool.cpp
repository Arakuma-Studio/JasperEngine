// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Fixed-size blocks for job envelopes, task states and dependency links (D4). Each thread
// keeps a cache per size class; a cache that runs dry refills from a mutex-guarded depot,
// and an empty depot carves a new chunk of 64 blocks. A cache that grows past its cap
// returns a batch to the depot (a block is often allocated on the submitting thread and
// freed on a worker). Chunks are never freed: after warm-up, acquire and release touch only
// the thread's cache, and a worker freeing a block during static destruction is safe.

#include "jobs/detail/JobCore.h"
#include "memory/MemoryStats.h"

#include <cstdlib>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::jobs;
using namespace jasperengine::jobs::detail;

namespace{

    constexpr size_t kClassCount = 3;
    constexpr uint32_t kBlocksPerChunk = 64;
    constexpr uint32_t kCacheMax = 256;
    constexpr uint32_t kTransferBatch = 64;
    constexpr size_t kChunkAlignment = 64;

    struct FreeBlock {
        FreeBlock* next;
    };

    constexpr size_t blockBytes(size_t slabClass){
        return slabClass == (size_t)SlabClass::Link ? kLinkSlabBytes
            : slabClass == (size_t)SlabClass::Job ? kJobSlabBytes : kTaskSlabBytes;
    }

    struct Depot {
        std::mutex mutex;
        FreeBlock* head = nullptr;
        uint32_t count = 0;
    };

    // Immortal: workers release blocks during static destruction.
    Depot* depots(){
        static Depot* all = new Depot[kClassCount];
        return all;
    }

    memory::TrackedBytes& slabBytes(){
        static memory::TrackedBytes* tracked = new memory::TrackedBytes("Jobs.Slabs", memory::Residency::Cpu);
        return *tracked;
    }

    memory::TrackedBytes& heapEnvelopeBytes(){
        static memory::TrackedBytes* tracked = new memory::TrackedBytes("Jobs.HeapEnvelopes", memory::Residency::Cpu);
        return *tracked;
    }

    std::atomic<uint64_t> heapEnvelopeCount{0};

    // Set (trivially, no destructor) once the thread's cache is gone: later calls on the
    // thread (other thread_local destructors) go straight to the depot.
    thread_local bool cacheGone = false;

    void giveToDepot(size_t slabClass, FreeBlock* first, FreeBlock* last, uint32_t count){
        Depot& depot = depots()[slabClass];
        std::lock_guard<std::mutex> lock(depot.mutex);
        last->next = depot.head;
        depot.head = first;
        depot.count += count;
    }

    struct ThreadCache {
        FreeBlock* head[kClassCount] = {};
        uint32_t count[kClassCount] = {};

        ~ThreadCache(){
            for (size_t c = 0; c < kClassCount; c++){
                if (!head[c])
                    continue;
                FreeBlock* last = head[c];
                while (last->next)
                    last = last->next;
                giveToDepot(c, head[c], last, count[c]);
                head[c] = nullptr;
                count[c] = 0;
            }
            cacheGone = true;
        }

        void refill(size_t slabClass){
            Depot& depot = depots()[slabClass];
            {
                std::lock_guard<std::mutex> lock(depot.mutex);
                uint32_t moved = 0;
                while (depot.head && moved < kTransferBatch){
                    FreeBlock* block = depot.head;
                    depot.head = block->next;
                    block->next = head[slabClass];
                    head[slabClass] = block;
                    moved++;
                }
                depot.count -= moved;
                count[slabClass] += moved;
            }
            if (head[slabClass])
                return;
            const size_t bytes = blockBytes(slabClass);
            auto* chunk = static_cast<unsigned char*>(::operator new(bytes * kBlocksPerChunk, std::align_val_t(kChunkAlignment)));
            slabBytes().add(bytes * kBlocksPerChunk);
            for (uint32_t i = 0; i < kBlocksPerChunk; i++){
                auto* block = reinterpret_cast<FreeBlock*>(chunk + i * bytes);
                block->next = head[slabClass];
                head[slabClass] = block;
            }
            count[slabClass] += kBlocksPerChunk;
        }

        void* acquire(size_t slabClass){
            if (!head[slabClass])
                refill(slabClass);
            FreeBlock* block = head[slabClass];
            head[slabClass] = block->next;
            count[slabClass]--;
            return block;
        }

        void release(size_t slabClass, void* memory){
            auto* block = static_cast<FreeBlock*>(memory);
            block->next = head[slabClass];
            head[slabClass] = block;
            if (++count[slabClass] <= kCacheMax)
                return;
            // keep the newest blocks (warm in this core's cache), return the oldest batch
            FreeBlock* keepLast = head[slabClass];
            for (uint32_t i = 1; i < kCacheMax - kTransferBatch; i++)
                keepLast = keepLast->next;
            FreeBlock* first = keepLast->next;
            keepLast->next = nullptr;
            FreeBlock* last = first;
            uint32_t moved = 1;
            while (last->next){
                last = last->next;
                moved++;
            }
            count[slabClass] -= moved;
            giveToDepot(slabClass, first, last, moved);
        }
    };

    thread_local ThreadCache threadCache;

    void* acquireFromDepot(size_t slabClass){
        Depot& depot = depots()[slabClass];
        {
            std::lock_guard<std::mutex> lock(depot.mutex);
            if (depot.head){
                FreeBlock* block = depot.head;
                depot.head = block->next;
                depot.count--;
                return block;
            }
        }
        slabBytes().add(blockBytes(slabClass));
        return ::operator new(blockBytes(slabClass), std::align_val_t(kChunkAlignment));
    }
}

void* jobs::detail::acquireSlab(SlabClass slabClass){
    const size_t c = (size_t)slabClass;
    if (cacheGone)
        return acquireFromDepot(c);
    return threadCache.acquire(c);
}

void jobs::detail::releaseSlab(SlabClass slabClass, void* block) noexcept{
    if (!block)
        return;
    const size_t c = (size_t)slabClass;
    if (cacheGone){
        auto* free = static_cast<FreeBlock*>(block);
        free->next = nullptr;
        giveToDepot(c, free, free, 1);
        return;
    }
    threadCache.release(c, block);
}

namespace{
    // The heap fallback keeps its size in front of the block, for the stats.
    constexpr size_t kHeapHeader = kChunkAlignment;
}

void* jobs::detail::acquireHeapEnvelope(size_t bytes){
    auto* memory = static_cast<unsigned char*>(::operator new(bytes + kHeapHeader, std::align_val_t(kChunkAlignment)));
    *reinterpret_cast<size_t*>(memory) = bytes;
    heapEnvelopeCount.fetch_add(1, std::memory_order_relaxed);
    heapEnvelopeBytes().add(bytes);
    return memory + kHeapHeader;
}

void jobs::detail::releaseHeapEnvelope(void* block) noexcept{
    if (!block)
        return;
    unsigned char* memory = static_cast<unsigned char*>(block) - kHeapHeader;
    heapEnvelopeBytes().subtract(*reinterpret_cast<size_t*>(memory));
    ::operator delete(memory, std::align_val_t(kChunkAlignment));
}

namespace jasperengine { namespace jobs { namespace detail {
    uint64_t heapEnvelopesAllocated(){
        return heapEnvelopeCount.load(std::memory_order_relaxed);
    }
}}}
