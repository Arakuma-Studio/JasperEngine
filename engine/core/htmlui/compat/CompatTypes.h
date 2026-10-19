// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATTYPES_H
#define COMPAT_COMPATTYPES_H

// Small value types shared by every part of the HTML/CSS compatibility layer
// (engine/core/htmlui/compat, phase-2 spec §3.4). No RmlUi or lexbor header here: the
// editor reaches the compat API through the shared engine library.

#include <iterator>
#include <initializer_list>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    // A read-only view of a contiguous array (std::span is C++20; the engine builds as C++17).
    template<typename T>
    class Span {
    public:
        constexpr Span() = default;
        constexpr Span(T* data, size_t size): ptr(data), count(size) {}
        template<size_t N>
        constexpr Span(T (&array)[N]): ptr(array), count(N) {}
        template<typename U, size_t N>
        constexpr Span(const std::array<U, N>& array): ptr(array.data()), count(N) {}
        template<typename U>
        Span(const std::vector<U>& vector): ptr(vector.data()), count(vector.size()) {}
        template<typename U>
        Span(std::vector<U>& vector): ptr(vector.data()), count(vector.size()) {}

        constexpr T* data() const { return ptr; }
        constexpr size_t size() const { return count; }
        constexpr bool empty() const { return count == 0; }
        constexpr T* begin() const { return ptr; }
        constexpr T* end() const { return ptr + count; }
        constexpr T& operator[](size_t index) const { return ptr[index]; }
        constexpr T& front() const { return ptr[0]; }
        constexpr T& back() const { return ptr[count - 1]; }
        constexpr Span subspan(size_t offset, size_t length) const { return Span(ptr + offset, length); }

    private:
        T* ptr = nullptr;
        size_t count = 0;
    };

    // A byte range in a registered source file (SourceFiles). file 0 means "no file"
    // (generated text: the user-agent stylesheet's companions, presentational hints).
    struct SourceRef {
        uint32_t file = 0;
        uint32_t offset = 0;   // bytes from the start of the file, after BOM removal
        uint32_t length = 0;   // bytes

        bool valid() const { return file != 0; }

        // the smallest range covering both (refs of different files: *this)
        SourceRef cover(const SourceRef& other) const {
            if (!valid())
                return other;
            if (!other.valid() || other.file != file)
                return *this;
            const uint32_t begin = offset < other.offset ? offset : other.offset;
            const uint32_t endA = offset + length;
            const uint32_t endB = other.offset + other.length;
            return SourceRef{file, begin, (endA > endB ? endA : endB) - begin};
        }

        bool operator==(const SourceRef& other) const {
            return file == other.file && offset == other.offset && length == other.length;
        }
        bool operator!=(const SourceRef& other) const { return !(*this == other); }
    };

    // Human position: 1-based line and column; the column counts Unicode code points.
    // 0 = unknown.
    struct SourcePos {
        uint32_t line = 0;
        uint32_t column = 0;

        bool operator==(const SourcePos& other) const { return line == other.line && column == other.column; }
        bool operator!=(const SourcePos& other) const { return !(*this == other); }
    };

    // A remote URL a document or stylesheet references (fonts, images, imports, Lottie):
    // what the editor importers download (phase-2 §3.13.3).
    struct RemoteReference {
        std::string url;    // as written (resolved against nothing: remote URLs are absolute)
        std::string kind;   // "google-fonts-css", "stylesheet", "font", "image", "lottie", "svg", "video" (phase 6)
        SourceRef ref;
    };

    // A list of SourceRefs that a compiled document keeps (the RCSS line maps, the RML source
    // map): what a std::vector<SourceRef> holds, in 2 to 4 bytes an entry instead of 12. An
    // entry is written as its difference from the entry before it: one byte of flags (the
    // same entry again; the file changes; the offset goes back; a length under 31), then the
    // file's and the offset's differences and a longer length as variable-length integers.
    // Every 32nd entry is also kept whole, so an index decodes at most 31 entries. Entries
    // are appended and read; they are not changed in place.
    class SourceRefList {
    public:
        class const_iterator {
        public:
            using iterator_category = std::input_iterator_tag;
            using value_type = SourceRef;
            using difference_type = std::ptrdiff_t;
            using pointer = const SourceRef*;
            using reference = SourceRef;

            SourceRef operator*() const { return current; }
            const SourceRef* operator->() const { return &current; }
            const_iterator& operator++(){
                index++;
                if (index < list->count)
                    at = decode(at, current);
                return *this;
            }
            bool operator==(const const_iterator& other) const { return index == other.index; }
            bool operator!=(const const_iterator& other) const { return index != other.index; }

        private:
            friend class SourceRefList;
            const SourceRefList* list = nullptr;
            const uint8_t* at = nullptr;
            size_t index = 0;
            SourceRef current;
        };

        SourceRefList() = default;
        SourceRefList(std::initializer_list<SourceRef> refs){
            for (const SourceRef& ref : refs)
                push_back(ref);
        }
        template<typename Iterator>
        SourceRefList(Iterator first, Iterator last){
            for (; first != last; ++first)
                push_back(*first);
        }

        size_t size() const { return count; }
        bool empty() const { return count == 0; }
        void clear(){
            bytes.clear();
            marks.clear();
            count = 0;
            last = SourceRef{};
        }
        void shrink_to_fit(){
            bytes.shrink_to_fit();
            marks.shrink_to_fit();
        }
        // the bytes the entries take (the encoded entries and the whole ones)
        size_t byteSize() const { return bytes.size() + marks.size() * sizeof(Mark); }

        void push_back(SourceRef ref){
            if (count % kInterval == 0)
                marks.push_back(Mark{(uint32_t)bytes.size(), last});
            if (count > 0 && ref == last){
                bytes.push_back(kSame);
            }else{
                const bool back = ref.offset < last.offset;
                const uint32_t distance = back ? last.offset - ref.offset : ref.offset - last.offset;
                uint8_t flags = (uint8_t)((ref.file != last.file ? kFile : 0) | (back ? kBack : 0));
                flags |= (uint8_t)((ref.length < kLongLength ? ref.length : kLongLength) << 3);
                bytes.push_back(flags);
                if (ref.file != last.file)
                    put(ref.file);
                put(distance);
                if (ref.length >= kLongLength)
                    put(ref.length);
            }
            last = ref;
            count++;
        }
        SourceRef operator[](size_t index) const {
            const Mark& mark = marks[index / kInterval];
            SourceRef ref = mark.previous;
            const uint8_t* at = bytes.data() + mark.at;
            for (size_t i = index / kInterval * kInterval; i <= index; i++)
                at = decode(at, ref);
            return ref;
        }
        SourceRef back() const { return last; }

        const_iterator begin() const {
            const_iterator it;
            it.list = this;
            if (count > 0)
                it.at = decode(bytes.data(), it.current);
            return it;
        }
        const_iterator end() const {
            const_iterator it;
            it.list = this;
            it.index = count;
            return it;
        }
        // (std::vector's insert at the end: the entries of another list or range)
        template<typename Iterator>
        void insert(const const_iterator&, Iterator first, Iterator last){
            for (; first != last; ++first)
                push_back(*first);
        }

        friend bool operator==(const SourceRefList& a, const SourceRefList& b){
            if (a.count != b.count)
                return false;
            const_iterator i = a.begin(), j = b.begin();
            for (; i != a.end(); ++i, ++j){
                if (*i != *j)
                    return false;
            }
            return true;
        }
        friend bool operator!=(const SourceRefList& a, const SourceRefList& b){ return !(a == b); }

    private:
        static constexpr size_t kInterval = 32;
        static constexpr uint8_t kSame = 1, kFile = 2, kBack = 4;
        static constexpr uint32_t kLongLength = 31;
        // the entry before entry k * kInterval and where that entry's bytes start
        struct Mark {
            uint32_t at;
            SourceRef previous;
        };

        void put(uint32_t value){
            while (value >= 0x80){
                bytes.push_back((uint8_t)(value | 0x80));
                value >>= 7;
            }
            bytes.push_back((uint8_t)value);
        }
        static const uint8_t* get(const uint8_t* at, uint32_t& value){
            value = 0;
            for (int shift = 0;; shift += 7){
                const uint8_t byte = *at++;
                value |= (uint32_t)(byte & 0x7F) << shift;
                if (!(byte & 0x80))
                    return at;
            }
        }
        // the entry written at `at`, given the entry before it in `ref`; where the next one starts
        static const uint8_t* decode(const uint8_t* at, SourceRef& ref){
            const uint8_t flags = *at++;
            if (flags & kSame)
                return at;
            if (flags & kFile)
                at = get(at, ref.file);
            uint32_t distance = 0;
            at = get(at, distance);
            ref.offset = (flags & kBack) ? ref.offset - distance : ref.offset + distance;
            ref.length = flags >> 3;
            if (ref.length == kLongLength)
                at = get(at, ref.length);
            return at;
        }

        std::vector<uint8_t> bytes;
        std::vector<Mark> marks;
        uint32_t count = 0;
        SourceRef last;
    };

    // RCSS output line -> source: lines[i] is output line i + 1 (RcssWriter, §3.9.10).
    struct RcssLineMap {
        SourceRefList lines;

        // the source of a 1-based output line; invalid when out of range or generated
        SourceRef at(uint32_t line) const {
            return (line >= 1 && line <= lines.size()) ? lines[line - 1] : SourceRef{};
        }
    };

    // RML output -> source (RmlSerializer, §3.7.6): elementRefs[n] is the element with
    // data-htmlui-loc="n"; lineRefs[i] is RML line i + 1.
    struct RmlSourceMap {
        SourceRefList elementRefs;
        SourceRefList lineRefs;

        SourceRef line(uint32_t rmlLine) const {
            return (rmlLine >= 1 && rmlLine <= lineRefs.size()) ? lineRefs[rmlLine - 1] : SourceRef{};
        }
    };

}

#endif //COMPAT_COMPATTYPES_H
