// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_ARRAY_H
#define JASPERENGINE_ARRAY_H

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace jasperengine {

template <typename T, std::size_t Size>
struct HybridArray {
#ifdef JASPERENGINE_EDITOR
    std::vector<T> values = std::vector<T>(Size);

    std::size_t size() const {
        return values.size();
    }

    T* data() {
        return values.data();
    }

    const T* data() const {
        return values.data();
    }

    bool validIndex(int index) const {
        return index >= 0;
    }

    T& operator[](std::size_t index) {
        if (index >= values.size()) {
            values.resize(index + 1);
        }
        return values[index];
    }

    const T& operator[](std::size_t index) const {
        return values[index];
    }

#else
    T values[Size]{};

    std::size_t size() const {
        return Size;
    }

    T* data() {
        return values;
    }

    const T* data() const {
        return values;
    }

    bool validIndex(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < Size;
    }

    T& operator[](std::size_t index) {
        return values[index];
    }

    const T& operator[](std::size_t index) const {
        return values[index];
    }
#endif
};

// An array that has no limit in any build, for what a model can have any number of (a mesh's
// submeshes and buffers). Size elements live inline, allocation-free; the first index past them moves
// them all to the heap, so data() stays one block. It is the editor's HybridArray there, and
// references to elements do not survive the array growing in any build.
#ifdef JASPERENGINE_EDITOR
template <typename T, std::size_t Size>
using SpillArray = HybridArray<T, Size>;
#else
template <typename T, std::size_t Size>
struct SpillArray {
    T values[Size]{};
    // every element once one went past Size; empty until then
    std::vector<T> spilled;

    std::size_t size() const {
        return spilled.empty() ? Size : spilled.size();
    }

    T* data() {
        return spilled.empty() ? values : spilled.data();
    }

    const T* data() const {
        return spilled.empty() ? values : spilled.data();
    }

    bool validIndex(int index) const {
        return index >= 0;
    }

    T& operator[](std::size_t index) {
        if (spilled.empty()) {
            if (index < Size) {
                return values[index];
            }
        } else if (index < spilled.size()) {
            return spilled[index];
        }
        grow(index + 1);
        return spilled[index];
    }

    const T& operator[](std::size_t index) const {
        return spilled.empty() ? values[index] : spilled[index];
    }

private:
    void grow(std::size_t count) {
        if (spilled.empty()) {
            spilled.reserve(std::max(count, Size * 2));
            for (T& value : values) {
                spilled.push_back(std::move(value));
                value = T();
            }
        }
        spilled.resize(count);
    }
};
#endif

}

#endif
