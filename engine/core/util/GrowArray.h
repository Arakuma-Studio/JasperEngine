// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_GROW_ARRAY_H
#define JASPERENGINE_GROW_ARRAY_H

#include <cstddef>
#include <utility>
#include <vector>

namespace jasperengine {

    // An index past it is an error, not content: the arrays of a component check the indices a
    // user, a file or an agent gives against it, so a typo is refused instead of allocated
    constexpr std::size_t ARRAY_INDEX_LIMIT = 65536;

    // A contiguous array of a component that holds what it is given: writing an index past its end
    // grows it to that index (the new elements value-initialised), reading one past its end gives a
    // default element and grows nothing. It never shrinks by itself. Growth moves the elements, as a
    // vector's does: keep indices, not references, across a write past the end.
    template <typename T>
    class GrowArray {
    public:
        GrowArray() = default;
        GrowArray(const GrowArray&) = default;
        GrowArray& operator=(const GrowArray&) = default;
        GrowArray(GrowArray&& other) noexcept: values(std::move(other.values)) {}
        GrowArray& operator=(GrowArray&& other) noexcept {
            values = std::move(other.values);
            return *this;
        }

        // the elements held: one past the highest index written, or what resize gave
        std::size_t stored() const {
            return values.size();
        }

        T* data() {
            return values.data();
        }

        const T* data() const {
            return values.data();
        }

        bool validIndex(int index) const {
            return index >= 0 && static_cast<std::size_t>(index) < ARRAY_INDEX_LIMIT;
        }

        T& operator[](std::size_t index) {
            if (index >= values.size()) {
                values.resize(index + 1);
            }
            return values[index];
        }

        const T& operator[](std::size_t index) const {
            return index < values.size() ? values[index] : defaultElement();
        }

        void resize(std::size_t count) {
            values.resize(count);
        }

        // nothing held, and no storage
        void clear() {
            std::vector<T>().swap(values);
        }

        void insert(std::size_t index, const T& value) {
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(index < values.size() ? index : values.size()), value);
        }

        void erase(std::size_t index) {
            if (index < values.size()) {
                values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }

        // the element at `from` taken out and put back at `to`
        void move(std::size_t from, std::size_t to) {
            if (from >= values.size() || to >= values.size() || from == to) {
                return;
            }
            T value = std::move(values[from]);
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(value));
        }

        // what its storage takes (component memory reports)
        std::size_t storageBytes() const {
            return values.capacity() * sizeof(T);
        }

        // what a read past the end gives: one per type, made on first use and never destroyed
        static const T& defaultElement() {
            static const T* const element = new T();
            return *element;
        }

    private:
        std::vector<T> values;
    };

}

#endif
