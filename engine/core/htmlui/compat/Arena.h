// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_ARENA_H
#define COMPAT_ARENA_H

// A bump allocator for the CSS syntax tree (phase-2 §3.9.1, p2-perf): tokens' texts, component
// value lists and selectors are allocated from the arena that is current on the thread and
// never freed one by one; the arena releases everything at once. Nothing allocated from it has
// a destructor.
//
// The current arena is the one of the innermost ArenaScope on the thread (a CssRewriter session
// holds one for its lifetime). Without a scope it is the thread's own arena, which lives until
// the thread ends: code that parses CSS outside a rewriter opens an ArenaScope around the use
// of what it parsed (the editor's style sheet importer does).

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace jasperengine::htmlui::compat {

    class JASPERENGINE_API Arena {
    public:
        Arena() = default;
        ~Arena(){ release(); }
        Arena(const Arena&) = delete;
        Arena& operator=(const Arena&) = delete;

        // `bytes` aligned to 8
        void* allocate(size_t bytes){
            char* at = (char*)(((uintptr_t)cursor + 7u) & ~(uintptr_t)7u);
            if (at <= end && (size_t)(end - at) >= bytes){
                cursor = at + bytes;
                return at;
            }
            return grow(bytes);
        }
        template<typename T>
        T* allocateArray(size_t count){ return (T*)allocate(count * sizeof(T)); }

        // a copy of the text (not terminated)
        const char* copy(const char* data, size_t size){
            if (size == 0)
                return "";
            char* at = cursor;
            if ((size_t)(end - cursor) >= size)
                cursor += size;
            else
                at = (char*)grow(size);
            std::memcpy(at, data, size);
            return at;
        }
        std::string_view copy(std::string_view text){ return std::string_view(copy(text.data(), text.size()), text.size()); }

        // frees everything allocated from it
        void release();
        size_t allocated() const { return total; }

        // the arena of the innermost ArenaScope of the thread, or the thread's own
        static Arena& current();

    private:
        friend class ArenaScope;
        struct Block {
            Block* next;
            size_t size;
        };
        void* grow(size_t bytes);

        Block* blocks = nullptr;
        char* cursor = nullptr;
        char* end = nullptr;
        size_t total = 0;
    };

    // makes an arena current on the thread for the scope's lifetime (scopes nest)
    class JASPERENGINE_API ArenaScope {
    public:
        explicit ArenaScope(Arena& arena);
        ~ArenaScope();
        ArenaScope(const ArenaScope&) = delete;
        ArenaScope& operator=(const ArenaScope&) = delete;

    private:
        Arena* previous;
    };

}

#endif //COMPAT_ARENA_H
