// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory::ScratchScope (Modules/Memory/Include/Memory/
// ScratchScope.h, Source/ScratchScope.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#ifndef MEMORY_SCRATCHSCOPE_H
#define MEMORY_SCRATCHSCOPE_H

#include "Export.h"
#include "memory/Arena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace jasperengine { namespace memory {

    // Stack memory for one block of code, from the calling thread's own scratch arena, so a
    // steady caller allocates nothing:
    //
    //     {
    //         memory::ScratchScope scratch;
    //         Candidate* candidates = scratch.allocateArrayForOverwrite<Candidate>(count);
    //         jobs::parallelFor(count, [&](size_t i){ candidates[i] = evaluate(i); });
    //     }   // everything returns here
    //
    // - Scopes nest on a thread, last opened first closed. A job that waits helps other jobs
    //   inline, and their scopes nest inside the waiter's: help is stack-ordered.
    // - Only the opening thread allocates from a scope; any thread may use the memory until it
    //   closes (hand units pointers, join before the closing brace, never across a submit).
    // - Automatic storage only. Trivially destructible types only.
    // - An allocation through a scope that is not the innermost one is served from the heap
    //   and freed when that scope closes: correct, but it costs a heap allocation.
    // - A thread's arena keeps what its outermost scopes need; a block more than twice the
    //   largest need of the last 1024 outermost scopes is released.
    class JASPERENGINE_API ScratchScope {
    public:
        ScratchScope();
        ~ScratchScope();

        ScratchScope(const ScratchScope&) = delete;
        ScratchScope& operator=(const ScratchScope&) = delete;
        static void* operator new(size_t) = delete;
        static void* operator new[](size_t) = delete;

        void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)){
            assert(alignment != 0 && (alignment & (alignment - 1)) == 0 && "ScratchScope alignment must be a power of two");
            if (arena && *openDepth == depth + 1)
                return arena->allocate(bytes, alignment);
            return allocateAside(bytes, alignment);
        }

        template<class T, class... Args>
        T* allocate(Args&&... args){
            static_assert(std::is_trivially_destructible<T>::value, "ScratchScope holds trivially destructible types");
            return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
        }

        template<class T>
        T* allocateArray(size_t count){
            static_assert(std::is_trivially_destructible<T>::value, "ScratchScope holds trivially destructible types");
            T* elements = static_cast<T*>(allocateArrayBytes(count, sizeof(T), alignof(T)));
            for (size_t i = 0; i < count; i++)
                ::new ((void*)(elements + i)) T();
            return elements;
        }

        template<class T>
        T* allocateArrayForOverwrite(size_t count){
            static_assert(std::is_trivially_destructible<T>::value, "ScratchScope holds trivially destructible types");
            return static_cast<T*>(allocateArrayBytes(count, sizeof(T), alignof(T)));
        }

        // Allocations served from the heap because the scope was not the innermost one, on
        // this thread so far.
        static uint64_t outOfOrderCount();

    private:
        void* allocateAside(size_t bytes, size_t alignment);
        void* allocateArrayBytes(size_t count, size_t elementBytes, size_t alignment){
            if (elementBytes != 0 && count > std::numeric_limits<size_t>::max() / elementBytes)
                throw std::bad_array_new_length();
            return allocate(count * elementBytes, alignment);
        }

        struct AsideBlock;

        // null on a thread whose scratch state has exited: every allocation goes aside
        Arena* arena;
        const uint32_t* openDepth;
        uint32_t depth;
        Arena::Marker mark;
        AsideBlock* aside;
    };

}}

#endif // MEMORY_SCRATCHSCOPE_H
