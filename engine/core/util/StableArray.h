// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_STABLE_ARRAY_H
#define JASPERENGINE_STABLE_ARRAY_H

#include "util/GrowArray.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace jasperengine {

    // A GrowArray whose elements are allocated one by one: growth never moves them, so a pointer to
    // one stays valid until it is erased, the array resized below it, cleared or destroyed (the
    // render code keeps pointers into a mesh's submeshes and buffers for a frame). Copies are deep;
    // a move takes the elements.
    template <typename T>
    class StableArray {
    public:
        StableArray() = default;

        StableArray(const StableArray& other) {
            copyFrom(other);
        }

        StableArray& operator=(const StableArray& other) {
            if (this != &other) {
                // the elements both hold are assigned in place (pointers to them stay valid)
                const std::size_t common = elements.size() < other.elements.size() ? elements.size() : other.elements.size();
                for (std::size_t i = 0; i < common; i++) {
                    *elements[i] = *other.elements[i];
                }
                elements.resize(common);
                for (std::size_t i = common; i < other.elements.size(); i++) {
                    elements.push_back(std::make_unique<T>(*other.elements[i]));
                }
            }
            return *this;
        }

        StableArray(StableArray&& other) noexcept: elements(std::move(other.elements)) {}

        StableArray& operator=(StableArray&& other) noexcept {
            elements = std::move(other.elements);
            return *this;
        }

        std::size_t stored() const {
            return elements.size();
        }

        bool validIndex(int index) const {
            return index >= 0 && static_cast<std::size_t>(index) < ARRAY_INDEX_LIMIT;
        }

        T& operator[](std::size_t index) {
            if (index >= elements.size()) {
                resize(index + 1);
            }
            return *elements[index];
        }

        const T& operator[](std::size_t index) const {
            return index < elements.size() ? *elements[index] : defaultElement();
        }

        void resize(std::size_t count) {
            if (count < elements.size()) {
                elements.resize(count);
                return;
            }
            elements.reserve(count);
            while (elements.size() < count) {
                elements.push_back(std::make_unique<T>());
            }
        }

        void clear() {
            std::vector<std::unique_ptr<T>>().swap(elements);
        }

        // the `count` elements from `first` in the opposite order: the elements are swapped, not
        // copied (a pointer follows its element to its new index)
        void reverse(std::size_t first, std::size_t count) {
            if (count < 2) {
                return;
            }
            if (first + count > elements.size()) {
                resize(first + count);
            }
            for (std::size_t a = first, b = first + count - 1; a < b; a++, b--) {
                std::swap(elements[a], elements[b]);
            }
        }

        void insert(std::size_t index, const T& value) {
            const std::size_t at = index < elements.size() ? index : elements.size();
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(at), std::make_unique<T>(value));
        }

        void erase(std::size_t index) {
            if (index < elements.size()) {
                elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }

        void move(std::size_t from, std::size_t to) {
            if (from >= elements.size() || to >= elements.size() || from == to) {
                return;
            }
            std::unique_ptr<T> element = std::move(elements[from]);
            elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(from));
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(to), std::move(element));
        }

        // what its elements and their pointers take (component memory reports; not what the
        // elements own themselves)
        std::size_t storageBytes() const {
            return elements.size() * sizeof(T) + elements.capacity() * sizeof(std::unique_ptr<T>);
        }

        static const T& defaultElement() {
            return GrowArray<T>::defaultElement();
        }

    private:
        void copyFrom(const StableArray& other) {
            elements.reserve(other.elements.size());
            for (const std::unique_ptr<T>& element : other.elements) {
                elements.push_back(std::make_unique<T>(*element));
            }
        }

        std::vector<std::unique_ptr<T>> elements;
    };

}

#endif
