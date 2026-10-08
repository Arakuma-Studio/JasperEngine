// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATTYPES_H
#define COMPAT_COMPATTYPES_H

// Small value types shared by every part of the HTML/CSS compatibility layer
// (engine/core/htmlui/compat, phase-2 spec §3.4). No RmlUi or lexbor header here: the
// editor reaches the compat API through the shared engine library.

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
        std::string kind;   // "google-fonts-css", "stylesheet", "font", "image", "lottie", "svg"
        SourceRef ref;
    };

    // RCSS output line -> source: lines[i] is output line i + 1 (RcssWriter, §3.9.10).
    struct RcssLineMap {
        std::vector<SourceRef> lines;

        // the source of a 1-based output line; invalid when out of range or generated
        SourceRef at(uint32_t line) const {
            return (line >= 1 && line <= lines.size()) ? lines[line - 1] : SourceRef{};
        }
    };

    // RML output -> source (RmlSerializer, §3.7.6): elementRefs[n] is the element with
    // data-htmlui-loc="n"; lineRefs[i] is RML line i + 1.
    struct RmlSourceMap {
        std::vector<SourceRef> elementRefs;
        std::vector<SourceRef> lineRefs;

        SourceRef line(uint32_t rmlLine) const {
            return (rmlLine >= 1 && rmlLine <= lineRefs.size()) ? lineRefs[rmlLine - 1] : SourceRef{};
        }
    };

}

#endif //COMPAT_COMPATTYPES_H
