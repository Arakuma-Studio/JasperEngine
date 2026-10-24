// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::Arena (Modules/Memory/Include/Memory/Arena.h,
// Source/Arena.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md. Changes: our naming and stats registry, no image-pin tracking, a
// std allocator (ArenaAllocator; std::pmr is outside the portable C++20 subset), FrameArena.

#ifndef MEMORY_ARENA_H
#define MEMORY_ARENA_H

#include "Export.h"
#include "memory/MemoryDebug.h"
#include "memory/MemoryStats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace jasperengine { namespace memory {

    // The first block an Arena acquires, unless its constructor names another size.
    constexpr size_t kArenaFirstBlockBytes = 64 * 1024;
    // The largest block one growth step acquires; one larger request gets a block of its own.
    constexpr size_t kArenaMaxBlockBytes = 4 * 1024 * 1024;

    // A linear allocator: allocations bump a cursor through chained blocks, and memory comes
    // back all at once, at rewind (to a marker) or reset (everything).
    //
    //     memory::Arena paths{"Navigation.Paths"};
    //     PathNode* nodes = paths.allocateArray<PathNode>(count);
    //     Request* request = paths.allocate<Request>(goal);   // ~Request runs at reset
    //     std::vector<int, memory::ArenaAllocator<int>> open{memory::ArenaAllocator<int>(paths)};
    //     paths.reset();                                       // once per frame
    //
    // - Threading: externally serialized, one user at a time on any thread (asserted in
    //   builds with assertions). Any thread may read memory it was handed.
    // - Lifetime: until the rewind or reset that covers it. Blocks chain and never move.
    // - Alignment: any power of two.
    // - Growth: the next retained block that fits, else a new block twice the previous one
    //   (at most kArenaMaxBlockBytes per step, or the request's size if larger). reset turns a
    //   chain into one block of the high-water size, so a steady frame allocates nothing.
    // - Destructors: allocate<T> records ~T for a type that is not trivially destructible;
    //   rewind and reset run them newest first.
    // - Stats: registered under its name as StatsKind::Arena, written when a block is
    //   acquired and at rewind and reset, never per allocation.
    // Under AddressSanitizer, memory not handed out is poisoned.
    class JASPERENGINE_API Arena {
    public:
        // A position to rewind to: valid until a rewind to an earlier position or a reset.
        struct Marker {
            uint32_t block = 0;
            uint32_t destructorCount = 0;
            size_t offset = 0;
        };

        explicit Arena(std::string_view statsName, size_t firstBlockBytes = kArenaFirstBlockBytes);
        ~Arena();

        Arena(const Arena&) = delete;
        Arena& operator=(const Arena&) = delete;

        // `alignment` is a power of two. Never null; std::bad_alloc when a block cannot be had.
        void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));

        // Constructs a T in the arena; its destructor runs at rewind or reset.
        template<class T, class... Args>
        T* allocate(Args&&... args);

        // `count` value-initialised elements of a trivially destructible T.
        template<class T>
        T* allocateArray(size_t count);
        // `count` elements left indeterminate, for arrays filled before they are read.
        template<class T>
        T* allocateArrayForOverwrite(size_t count);
        template<class T>
        T* copyArray(const T* source, size_t count);

        Marker marker() const;
        // Runs the destructors recorded after `marker`, newest first, and moves the cursor
        // back to it. Every block is kept.
        void rewind(Marker marker);
        // Runs every recorded destructor and empties the arena; a chain of blocks becomes
        // one block of the high-water size.
        void reset();

        // Bytes handed out since the last reset, alignment padding included.
        size_t bytesUsed() const;
        size_t bytesReserved() const;
        size_t blockCount() const { return blocks.size(); }

    private:
        friend class ScratchScope;

        struct Block {
            unsigned char* data = nullptr;
            size_t capacity = 0;
            // the cursor offset; for the current block, as of the last block switch
            size_t used = 0;
        };

        struct DestructorRecord {
            void* object = nullptr;
            void (*destroy)(void*) = nullptr;
        };

        template<class T>
        static void destroyObject(void* object){ static_cast<T*>(object)->~T(); }

        void* tryBump(size_t bytes, size_t alignment);
        static void retireRange(unsigned char* begin, size_t bytes);
        void* allocateFromNextBlock(size_t bytes, size_t alignment);
        void* allocateArrayBytes(size_t count, size_t elementBytes, size_t alignment);
        void reserveDestructorRecord();
        void recordDestructor(void* object, void (*destroy)(void*));
        void runDestructorsAbove(uint32_t count);
        void publishStats();

        std::vector<Block> blocks;
        std::vector<DestructorRecord> destructors;
        // the bump cursor and the end of the current block; with no block every request misses
        uintptr_t limit = 0;
        size_t current = 0;
        uintptr_t cursor = 1;
        size_t usedBeforeCurrent = 0;
        size_t peakSinceReset = 0;
        size_t highWater = 0;
        size_t firstBlockBytes;
        detail::StatsRecord* record;
        detail::SerialUseCheck serialUse;
    };

    inline void* Arena::tryBump(size_t bytes, size_t alignment){
        const uintptr_t aligned = (cursor + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
        if (aligned > limit || bytes > limit - aligned)
            return nullptr;
        cursor = aligned + bytes;
        void* memory = reinterpret_cast<void*>(aligned);
        JE_MEMORY_UNPOISON(memory, bytes);
#if JE_MEMORY_CHECKS
        std::memset(memory, 0xCD, bytes);
#endif
        return memory;
    }

    inline void* Arena::allocate(size_t bytes, size_t alignment){
        assert(alignment != 0 && (alignment & (alignment - 1)) == 0 && "Arena alignment must be a power of two");
        detail::SerialUseScope use(serialUse, "Arena");
        void* memory = tryBump(bytes, alignment);
        if (!memory)
            memory = allocateFromNextBlock(bytes, alignment);
        return memory;
    }

    template<class T, class... Args>
    T* Arena::allocate(Args&&... args){
        void* memory = allocate(sizeof(T), alignof(T));
        if constexpr (std::is_trivially_destructible<T>::value){
            return ::new (memory) T(std::forward<Args>(args)...);
        }else{
            reserveDestructorRecord();
            T* object = ::new (memory) T(std::forward<Args>(args)...);
            recordDestructor(object, &destroyObject<T>);
            return object;
        }
    }

    template<class T>
    T* Arena::allocateArray(size_t count){
        static_assert(std::is_trivially_destructible<T>::value, "Arena arrays hold trivially destructible types");
        T* elements = static_cast<T*>(allocateArrayBytes(count, sizeof(T), alignof(T)));
        for (size_t i = 0; i < count; i++)
            ::new ((void*)(elements + i)) T();
        return elements;
    }

    template<class T>
    T* Arena::allocateArrayForOverwrite(size_t count){
        static_assert(std::is_trivially_destructible<T>::value, "Arena arrays hold trivially destructible types");
        T* elements = static_cast<T*>(allocateArrayBytes(count, sizeof(T), alignof(T)));
        for (size_t i = 0; i < count; i++)
            ::new ((void*)(elements + i)) T;
        return elements;
    }

    template<class T>
    T* Arena::copyArray(const T* source, size_t count){
        static_assert(std::is_trivially_destructible<T>::value, "Arena arrays hold trivially destructible types");
        T* elements = static_cast<T*>(allocateArrayBytes(count, sizeof(T), alignof(T)));
        std::uninitialized_copy(source, source + count, elements);
        return elements;
    }

    // A std allocator over an Arena: deallocate does nothing (the memory returns at the
    // arena's rewind or reset). Works on every platform (std::pmr is outside the portable subset).
    template<class T>
    class ArenaAllocator {
    public:
        using value_type = T;

        explicit ArenaAllocator(Arena& arena) noexcept: arena(&arena) {}
        template<class U>
        ArenaAllocator(const ArenaAllocator<U>& other) noexcept: arena(other.getArena()) {}

        T* allocate(size_t count){
            if (count > SIZE_MAX / sizeof(T))
                throw std::bad_array_new_length();
            return static_cast<T*>(arena->allocate(count * sizeof(T), alignof(T)));
        }
        void deallocate(T*, size_t) noexcept {}

        Arena* getArena() const noexcept { return arena; }

        template<class U>
        bool operator==(const ArenaAllocator<U>& other) const noexcept { return arena == other.getArena(); }
        template<class U>
        bool operator!=(const ArenaAllocator<U>& other) const noexcept { return arena != other.getArena(); }

    private:
        Arena* arena;
    };

    // One arena per frame in flight: what a frame allocates stays valid until the start of
    // the frame kFramesInFlight frames later, when the Engine resets its arena. Main thread.
    class JASPERENGINE_API FrameArena {
    public:
        static constexpr int kFramesInFlight = 2;

        explicit FrameArena(std::string_view statsName);

        Arena& current(){ return *arenas[index]; }
        // The Engine calls it at the start of every frame.
        void beginFrame();

    private:
        std::unique_ptr<Arena> arenas[kFramesInFlight];
        int index = 0;
    };

    // The engine's frame arena ("Engine.FrameArena"), reset at the start of every frame.
    JASPERENGINE_API FrameArena& frameArena();

}}

#endif // MEMORY_ARENA_H
