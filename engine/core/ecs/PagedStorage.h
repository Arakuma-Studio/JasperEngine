// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_PAGEDSTORAGE_H
#define ECS_PAGEDSTORAGE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>
#include <vector>
#include "Entity.h"

namespace jasperengine::ecs {

    // Elements per page: about 16 KiB, at least 64, a power of two so that an index splits
    // into page and offset with a shift and a mask
    constexpr size_t pageCapacityFor(size_t elementSize){
        size_t target = 16384 / (elementSize ? elementSize : 1);
        size_t capacity = 1;
        while (capacity * 2 <= target){
            capacity *= 2;
        }
        return capacity < 64 ? 64 : capacity;
    }

    constexpr unsigned log2Of(size_t powerOfTwo){
        unsigned bits = 0;
        while ((size_t(1) << bits) < powerOfTwo){
            bits++;
        }
        return bits;
    }

    // Fixed-size pages of raw storage for U. Pages never move, so a pointer into one stays
    // valid until its owner releases the page. The owner constructs and destroys the elements.
    template<typename U>
    class PagedArray {
    public:
        static constexpr size_t kPageCapacity = pageCapacityFor(sizeof(U));
        static constexpr unsigned kPageShift = log2Of(kPageCapacity);
        static constexpr size_t kPageMask = kPageCapacity - 1;

    private:
        std::vector<U*> pages;

        static U* allocatePage(){
            if constexpr (alignof(U) > __STDCPP_DEFAULT_NEW_ALIGNMENT__){
                return static_cast<U*>(::operator new(sizeof(U) * kPageCapacity, std::align_val_t(alignof(U))));
            }else{
                return static_cast<U*>(::operator new(sizeof(U) * kPageCapacity));
            }
        }

        static void freePage(U* page){
            if constexpr (alignof(U) > __STDCPP_DEFAULT_NEW_ALIGNMENT__){
                ::operator delete(page, std::align_val_t(alignof(U)));
            }else{
                ::operator delete(page);
            }
        }

    public:
        PagedArray() = default;
        PagedArray(const PagedArray&) = delete;
        PagedArray& operator=(const PagedArray&) = delete;

        ~PagedArray(){
            for (U* page : pages){
                freePage(page);
            }
        }

        U* slot(size_t index) const {
            return pages[index >> kPageShift] + (index & kPageMask);
        }

        U& operator[](size_t index) const {
            return *slot(index);
        }

        size_t capacity() const {
            return pages.size() * kPageCapacity;
        }

        size_t pageCount() const {
            return pages.size();
        }

        U* page(size_t pageIndex) const {
            return pages[pageIndex];
        }

        void reserve(size_t count){
            while (capacity() < count){
                pages.push_back(allocatePage());
            }
        }

        // Frees the pages past the one that holds element count-1, keeping one spare page
        // so that an array that shrinks and grows around a page boundary does not churn
        void shrinkTo(size_t count){
            const size_t needed = (count + kPageCapacity - 1) / kPageCapacity + 1;
            while (pages.size() > needed){
                freePage(pages.back());
                pages.pop_back();
            }
        }

        size_t bytes() const {
            return pages.size() * kPageCapacity * sizeof(U) + pages.capacity() * sizeof(U*);
        }
    };

    // Random-access iterator over the constructed elements of a PagedArray, for the
    // standard algorithms (std::rotate)
    template<typename U>
    class PagedIterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = U;
        using difference_type = std::ptrdiff_t;
        using pointer = U*;
        using reference = U&;

    private:
        const PagedArray<U>* array = nullptr;
        difference_type index = 0;

    public:
        PagedIterator() = default;
        PagedIterator(const PagedArray<U>* array, size_t index): array(array), index((difference_type)index) {}

        reference operator*() const { return (*array)[(size_t)index]; }
        pointer operator->() const { return array->slot((size_t)index); }
        reference operator[](difference_type n) const { return (*array)[(size_t)(index + n)]; }

        PagedIterator& operator++(){ ++index; return *this; }
        PagedIterator operator++(int){ PagedIterator old = *this; ++index; return old; }
        PagedIterator& operator--(){ --index; return *this; }
        PagedIterator operator--(int){ PagedIterator old = *this; --index; return old; }
        PagedIterator& operator+=(difference_type n){ index += n; return *this; }
        PagedIterator& operator-=(difference_type n){ index -= n; return *this; }

        friend PagedIterator operator+(PagedIterator it, difference_type n){ return it += n; }
        friend PagedIterator operator+(difference_type n, PagedIterator it){ return it += n; }
        friend PagedIterator operator-(PagedIterator it, difference_type n){ return it -= n; }
        friend difference_type operator-(const PagedIterator& a, const PagedIterator& b){ return a.index - b.index; }

        friend bool operator==(const PagedIterator& a, const PagedIterator& b){ return a.index == b.index; }
        friend bool operator!=(const PagedIterator& a, const PagedIterator& b){ return a.index != b.index; }
        friend bool operator<(const PagedIterator& a, const PagedIterator& b){ return a.index < b.index; }
        friend bool operator>(const PagedIterator& a, const PagedIterator& b){ return a.index > b.index; }
        friend bool operator<=(const PagedIterator& a, const PagedIterator& b){ return a.index <= b.index; }
        friend bool operator>=(const PagedIterator& a, const PagedIterator& b){ return a.index >= b.index; }
    };

    // Entity -> uint32 table in pages of 4096 entries: a page is allocated when its first
    // entity is added and freed when its last one goes. Iteration is in ascending entity order.
    class SparseIndex {
    public:
        static constexpr uint32_t kNone = 0xFFFFFFFFu;
        static constexpr unsigned kPageBits = 12;
        static constexpr size_t kPageSize = size_t(1) << kPageBits;
        static constexpr size_t kPageMask = kPageSize - 1;

    private:
        std::vector<uint32_t*> pages;
        std::vector<uint32_t> liveCounts; // parallel to pages
        size_t allocatedPages = 0;

        static uint32_t* allocatePage(){
            uint32_t* page = new uint32_t[kPageSize];
            std::memset(page, 0xFF, kPageSize * sizeof(uint32_t));
            return page;
        }

        void copyFrom(const SparseIndex& other){
            pages.assign(other.pages.size(), nullptr);
            liveCounts = other.liveCounts;
            allocatedPages = other.allocatedPages;
            for (size_t p = 0; p < other.pages.size(); p++){
                if (other.pages[p]){
                    pages[p] = new uint32_t[kPageSize];
                    std::memcpy(pages[p], other.pages[p], kPageSize * sizeof(uint32_t));
                }
            }
        }

    public:
        SparseIndex() = default;

        SparseIndex(const SparseIndex& other){
            copyFrom(other);
        }

        SparseIndex& operator=(const SparseIndex& other){
            if (this != &other){
                clear();
                copyFrom(other);
            }
            return *this;
        }

        ~SparseIndex(){
            clear();
        }

        uint32_t find(Entity entity) const {
            const size_t p = entity >> kPageBits;
            if (p >= pages.size() || !pages[p]){
                return kNone;
            }
            return pages[p][entity & kPageMask];
        }

        bool contains(Entity entity) const {
            return find(entity) != kNone;
        }

        // `entity` must not be in the table yet
        void insert(Entity entity, uint32_t value){
            const size_t p = entity >> kPageBits;
            if (p >= pages.size()){
                pages.resize(p + 1, nullptr);
                liveCounts.resize(p + 1, 0);
            }
            if (!pages[p]){
                pages[p] = allocatePage();
                allocatedPages++;
            }
            pages[p][entity & kPageMask] = value;
            liveCounts[p]++;
        }

        // `entity` must be in the table
        void assign(Entity entity, uint32_t value){
            pages[entity >> kPageBits][entity & kPageMask] = value;
        }

        // `entity` must be in the table
        void erase(Entity entity){
            const size_t p = entity >> kPageBits;
            pages[p][entity & kPageMask] = kNone;
            if (--liveCounts[p] == 0){
                delete[] pages[p];
                pages[p] = nullptr;
                allocatedPages--;
                // trailing empty slots of the page table go too
                while (!pages.empty() && !pages.back()){
                    pages.pop_back();
                    liveCounts.pop_back();
                }
            }
        }

        void clear(){
            for (uint32_t* page : pages){
                delete[] page;
            }
            pages.clear();
            liveCounts.clear();
            allocatedPages = 0;
        }

        size_t pageCount() const {
            return allocatedPages;
        }

        size_t bytes() const {
            return allocatedPages * kPageSize * sizeof(uint32_t)
                + pages.capacity() * sizeof(uint32_t*) + liveCounts.capacity() * sizeof(uint32_t);
        }

        // fn(Entity, uint32_t value) in ascending entity order; fn returns false to stop
        template<typename F>
        bool forEach(F&& fn) const {
            for (size_t p = 0; p < pages.size(); p++){
                const uint32_t* page = pages[p];
                if (!page){
                    continue;
                }
                uint32_t remaining = liveCounts[p];
                for (size_t i = 0; i < kPageSize && remaining > 0; i++){
                    if (page[i] != kNone){
                        remaining--;
                        if (!fn((Entity)((p << kPageBits) | i), page[i])){
                            return false;
                        }
                    }
                }
            }
            return true;
        }

        // the lowest entity in the table, false when it is empty
        bool first(Entity& entity) const {
            bool found = false;
            forEach([&](Entity e, uint32_t){ entity = e; found = true; return false; });
            return found;
        }
    };

}

#endif // ECS_PAGEDSTORAGE_H
