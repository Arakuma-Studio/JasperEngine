// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::ObjectPool (Modules/Memory/Include/Memory/ObjectPool.h,
// Source/ObjectPool.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#ifndef MEMORY_OBJECTPOOL_H
#define MEMORY_OBJECTPOOL_H

#include "Export.h"
#include "memory/MemoryDebug.h"
#include "memory/MemoryStats.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine { namespace memory {

    namespace detail {

        // The untyped storage behind ObjectPool<T>: fixed-size slots in blocks that never
        // move, an intrusive last-in first-out freelist through the free slots, one live bit
        // per slot (checked builds) at the end of each block.
        class JASPERENGINE_API ObjectPoolStorage {
        public:
            ObjectPoolStorage(std::string_view statsName, size_t objectBytes, size_t objectAlignment, bool assertNoLeaks);
            ~ObjectPoolStorage();

            ObjectPoolStorage(const ObjectPoolStorage&) = delete;
            ObjectPoolStorage& operator=(const ObjectPoolStorage&) = delete;

            void* take(){
                SerialUseScope use(serialUse, "ObjectPool");
                void* slot = freeHead;
                if (slot){
                    // the link is the one word of a free slot left unpoisoned
                    std::memcpy(&freeHead, slot, sizeof(void*));
                }else if (cursor != limit){
                    slot = cursor;
                    cursor += slotBytes;
                }else{
                    slot = takeFromNewBlock();
                }
                JE_MEMORY_UNPOISON(slot, slotBytes);
#if JE_MEMORY_CHECKS
                markLive(slot);
#endif
                liveCount++;
                if (liveCount > peakLive)
                    peakLive = liveCount;
                if (++operations % kPublishEvery == 0)
                    publishStats();
                return slot;
            }

            // Checked builds: asserts that `slot` holds a live object of this pool and marks
            // it free, before its destructor runs.
            void checkLiveAndMarkFree(void* slot);

            void giveBack(void* slot){
                SerialUseScope use(serialUse, "ObjectPool");
#if JE_MEMORY_CHECKS
                std::memset(slot, 0xDD, slotBytes);
#endif
                std::memcpy(slot, &freeHead, sizeof(void*));
                freeHead = slot;
                JE_MEMORY_POISON(static_cast<unsigned char*>(slot) + sizeof(void*), slotBytes - sizeof(void*));
                liveCount--;
                if (++operations % kPublishEvery == 0)
                    publishStats();
            }

            bool isDestroying() const { return destroying; }
            bool claimWhileDestroying(void* slot);
            void retireWhileDestroying(void* slot);
            // Destroys every live object once, in address order (asserting, in checked
            // builds and unless the pool was made without the check, that none was left).
            void destroyLiveObjects(void (*destroy)(void*)) noexcept;

            size_t getLiveCount() const { return liveCount; }
            size_t getSlotBytes() const { return slotBytes; }

        private:
            static constexpr uint32_t kPublishEvery = 64;

            struct BlockDescriptor {
                unsigned char* begin = nullptr;
                uint32_t slotCount = 0;
                uint64_t* bitmap = nullptr;
            };

            struct SlotLocation {
                size_t block = 0;
                size_t slot = 0;
            };

            void* takeFromNewBlock();
            size_t usedSlots(const BlockDescriptor& block) const;
            SlotLocation locate(const void* slot) const;
            void markLive(void* slot);
            void deriveLiveSet();
            void publishStats();

            // sorted by begin: block addresses are not monotonic
            std::vector<BlockDescriptor> blocks;
            void* freeHead = nullptr;
            unsigned char* cursor = nullptr;
            unsigned char* limit = nullptr;
            unsigned char* newestBegin = nullptr;
            size_t slotBytes;
            size_t blockAlignment;
            size_t newestSlotCount = 0;
            size_t liveCount = 0;
            size_t peakLive = 0;
            size_t reservedBytes = 0;
            uint64_t operations = 0;
            bool destroying = false;
            bool assertNoLeaks;
            StatsRecord* record;
            SerialUseCheck serialUse;
        };

    }

    // Fixed-size slots for objects of one type that need a stable address: create constructs
    // an object in a free slot, destroy destroys it and frees the slot for the next create.
    //
    //     memory::ObjectPool<PathRequest> requests{"Navigation.PathRequests"};
    //     PathRequest* request = requests.create(from, to);   // stable until destroy
    //     requests.destroy(request);
    //
    // - O(1) create and destroy; freed slots are reused last-in first-out.
    // - Blocks never move and are freed only with the pool. The first block holds
    //   max(16, 16 KiB / slot) slots and each new one twice the previous (up to 4 MiB).
    // - Externally serialized (asserted in checked builds). Checked builds also assert a
    //   double destroy, a foreign pointer, and objects still alive when the pool is destroyed
    //   (a leak; `assertNoLeaks` false for a pool that relies on destroying them); the pool
    //   destroys those once either way, in address order.
    // - Stats: registered as StatsKind::Pool, written at block acquisition and every 64th
    //   operation; getLiveCount() is exact at any time.
    template<class T>
    class ObjectPool {
    public:
        explicit ObjectPool(std::string_view statsName, bool assertNoLeaks = true):
            storage(statsName, sizeof(T), alignof(T), assertNoLeaks) {}
        ~ObjectPool(){ storage.destroyLiveObjects(&destroyObject); }

        ObjectPool(const ObjectPool&) = delete;
        ObjectPool& operator=(const ObjectPool&) = delete;

        template<class... Args>
        T* create(Args&&... args){
            void* slot = storage.take();
            try{
                return ::new (slot) T(std::forward<Args>(args)...);
            }catch(...){
                storage.checkLiveAndMarkFree(slot);
                storage.giveBack(slot);
                throw;
            }
        }

        // null does nothing, as with delete
        void destroy(T* object){
            if (!object)
                return;
            if (storage.isDestroying()){
                if (storage.claimWhileDestroying(object)){
                    object->~T();
                    storage.retireWhileDestroying(object);
                }
                return;
            }
#if JE_MEMORY_CHECKS
            storage.checkLiveAndMarkFree(object);
#endif
            object->~T();
            storage.giveBack(object);
        }

        size_t getLiveCount() const { return storage.getLiveCount(); }

    private:
        static void destroyObject(void* object){ static_cast<T*>(object)->~T(); }

        detail::ObjectPoolStorage storage;
    };

}}

#endif // MEMORY_OBJECTPOOL_H
