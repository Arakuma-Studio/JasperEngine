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
#include <initializer_list>
#include <new>
#include <string_view>
#include <utility>

namespace jasperengine::htmlui::compat {

    // What a thread keeps from one rewrite to the next, so that its memory is not mapped again
    // (and page-faulted) for every style sheet: the blocks its arenas release, up to
    // kArenaKeptBytes, and the storage of a rewriter session's big lists and of the RCSS
    // writer's output (SpareStorage), up to kSpareKeptBytes together. What would exceed a cap
    // is freed instead of kept.
    constexpr size_t kArenaKeptBytes = 4 * 1024 * 1024;
    constexpr size_t kSpareKeptBytes = 4 * 1024 * 1024;

    // The byte count of the spares a thread keeps (SpareStorage: CssRewriter's session lists,
    // RcssWriter's output buffer), within kSpareKeptBytes
    class JASPERENGINE_API SpareStorage {
    public:
        // a spare of `bytes` replaces one of `previous` bytes: true (and counted) when the
        // thread's total stays within kSpareKeptBytes, false (nothing counted) otherwise
        static bool keep(size_t previous, size_t bytes);
        // a spare of `bytes` is taken back into use (no longer kept)
        static void take(size_t bytes);
        // the bytes the thread keeps (tests)
        static size_t kept();
    };

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

    // A list in the current Arena with the std::vector operations the compatibility layer uses.
    // A copy copies the elements (and so their own lists); nothing is freed, and there is no
    // destructor: T is destroyed by forgetting it, and moved as its bytes. Growing leaves the
    // old storage to the arena, so lists are reserved to their size where it is known.
    // 16 bytes.
    template<typename T>
    class ArenaList {
    public:
        using value_type = T;
        using iterator = T*;
        using const_iterator = const T*;

        constexpr ArenaList() noexcept = default;
        ArenaList(const ArenaList& other){
            if (other.size_ > 0){
                data_ = Arena::current().allocateArray<T>(other.size_);
                size_ = capacity_ = other.size_;
                copyInto(data_, other.data_, other.data_ + other.size_);
            }
        }
        ArenaList(ArenaList&& other) noexcept: data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
            other.data_ = nullptr;
            other.size_ = other.capacity_ = 0;
        }
        ArenaList(std::initializer_list<T> values): ArenaList(values.begin(), values.end()) {}
        // the elements [first, first + count) moved in as their bytes: the originals are
        // forgotten, not destroyed (a parser's scratch stack hands its items over this way)
        static ArenaList movedFrom(T* first, size_t count){
            ArenaList list;
            if (count > 0){
                list.data_ = Arena::current().allocateArray<T>(count);
                std::memcpy((void*)list.data_, (const void*)first, count * sizeof(T));
                list.size_ = list.capacity_ = (uint32_t)count;
            }
            return list;
        }
        ArenaList(const T* first, const T* last){
            if (last > first){
                size_ = capacity_ = (uint32_t)(last - first);
                data_ = Arena::current().allocateArray<T>(size_);
                copyInto(data_, first, last);
            }
        }
        ArenaList& operator=(const ArenaList& other){
            if (this != &other)
                assign(other.data_, other.data_ + other.size_);
            return *this;
        }
        ArenaList& operator=(ArenaList&& other) noexcept {
            if (this != &other){
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.data_ = nullptr;
                other.size_ = other.capacity_ = 0;
            }
            return *this;
        }

        size_t size() const noexcept { return size_; }
        size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return size_ == 0; }
        T* data() noexcept { return data_; }
        const T* data() const noexcept { return data_; }
        T* begin() noexcept { return data_; }
        const T* begin() const noexcept { return data_; }
        T* end() noexcept { return data_ + size_; }
        const T* end() const noexcept { return data_ + size_; }
        T& operator[](size_t index) noexcept { return data_[index]; }
        const T& operator[](size_t index) const noexcept { return data_[index]; }
        T& front() noexcept { return data_[0]; }
        const T& front() const noexcept { return data_[0]; }
        T& back() noexcept { return data_[size_ - 1]; }
        const T& back() const noexcept { return data_[size_ - 1]; }

        void clear() noexcept { size_ = 0; }
        void reserve(size_t count){
            if (count > capacity_)
                grow(count);
        }
        void resize(size_t count){
            if (count > capacity_)
                grow(count);
            for (size_t i = size_; i < count; i++)
                new ((void*)(data_ + i)) T();
            size_ = (uint32_t)count;
        }
        T& emplace_back(){
            if (size_ == capacity_)
                grow(capacity_ ? (size_t)capacity_ * 2 : 4);
            return *new ((void*)(data_ + size_++)) T();
        }
        void push_back(const T& value){
            // (the value may be one of the list's own: copied before the list grows)
            Slot slot;
            new ((void*)slot.bytes) T(value);
            if (size_ == capacity_)
                grow(capacity_ ? (size_t)capacity_ * 2 : 4);
            std::memcpy((void*)(data_ + size_++), slot.bytes, sizeof(T));
        }
        void push_back(T&& value){
            if (size_ == capacity_){
                // (the value may be one of the list's own: moved out before the list grows)
                Slot slot;
                new ((void*)slot.bytes) T(std::move(value));
                grow(capacity_ ? (size_t)capacity_ * 2 : 4);
                std::memcpy((void*)(data_ + size_++), slot.bytes, sizeof(T));
                return;
            }
            new ((void*)(data_ + size_++)) T(std::move(value));
        }
        T& emplace_back(const T& value){
            push_back(value);
            return back();
        }
        T& emplace_back(T&& value){
            push_back(std::move(value));
            return back();
        }
        void pop_back() noexcept { size_--; }
        T* insert(const T* position, const T& value){ return insert(position, &value, &value + 1); }
        T* insert(const T* position, const T* first, const T* last){
            const size_t at = (size_t)(position - data_);
            const size_t count = (size_t)(last - first);
            if (count == 0)
                return data_ + at;
            // (the values may be the list's own: copied before anything moves)
            T* copies = Arena::current().allocateArray<T>(count);
            copyInto(copies, first, last);
            if (size_ + count > capacity_)
                grow((size_t)capacity_ * 2 > (size_t)size_ + count ? (size_t)capacity_ * 2 : (size_t)size_ + count);
            std::memmove((void*)(data_ + at + count), (const void*)(data_ + at), (size_ - at) * sizeof(T));
            std::memcpy((void*)(data_ + at), (const void*)copies, count * sizeof(T));
            size_ += (uint32_t)count;
            return data_ + at;
        }
        T* erase(const T* position){ return erase(position, position + 1); }
        T* erase(const T* first, const T* last){
            const size_t at = (size_t)(first - data_);
            const size_t count = (size_t)(last - first);
            if (count > 0){
                std::memmove((void*)(data_ + at), (const void*)(data_ + at + count), (size_ - at - count) * sizeof(T));
                size_ -= (uint32_t)count;
            }
            return data_ + at;
        }
        void assign(const T* first, const T* last){
            const size_t count = (size_t)(last - first);
            // (a range of the list itself is copied before the list changes)
            const bool own = first >= data_ && first < data_ + capacity_;
            if (count <= capacity_ && !own){
                copyInto(data_, first, last);
                size_ = (uint32_t)count;
                return;
            }
            T* storage = count > 0 ? Arena::current().allocateArray<T>(count) : nullptr;
            copyInto(storage, first, last);
            data_ = storage;
            size_ = capacity_ = (uint32_t)count;
        }
        void swap(ArenaList& other) noexcept {
            std::swap(data_, other.data_);
            std::swap(size_, other.size_);
            std::swap(capacity_, other.capacity_);
        }

    private:
        struct Slot {
            alignas(T) unsigned char bytes[sizeof(T)];
        };
        void grow(size_t count){
            // (count > capacity_) the elements move as their bytes
            T* storage = Arena::current().allocateArray<T>(count);
            if (size_ > 0)
                std::memcpy((void*)storage, (const void*)data_, size_ * sizeof(T));
            data_ = storage;
            capacity_ = (uint32_t)count;
        }
        // copies of [first, last) made in uninitialized storage
        static void copyInto(T* to, const T* first, const T* last){
            for (; first != last; ++first, ++to)
                new ((void*)to) T(*first);
        }

        T* data_ = nullptr;
        uint32_t size_ = 0;
        uint32_t capacity_ = 0;
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
