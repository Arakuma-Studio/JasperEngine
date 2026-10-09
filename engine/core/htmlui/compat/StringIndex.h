// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_STRINGINDEX_H
#define COMPAT_STRINGINDEX_H

// Internal to the compat layer: a fixed set of names (table rows, keyword lists) to a value,
// built once and looked up on every declaration and selector. Open addressing over an FNV-1a
// hash: a lookup hashes the name and compares one or two keys (std::unordered_map hashes with
// murmur, divides by a prime and follows a bucket list). Keys are views: they must outlive the
// index (string literals, static tables).

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    class StringIndex {
    public:
        static constexpr uint32_t npos = 0xFFFFFFFFu;

        StringIndex() = default;

        // keys with their values; a key given twice keeps its first value
        explicit StringIndex(const std::vector<std::pair<std::string_view, uint32_t>>& entries){
            size_t capacity = 16;
            while (capacity < entries.size() * 2)
                capacity *= 2;
            slots.assign(capacity, Slot{});
            mask = capacity - 1;
            for (const auto& entry : entries){
                size_t i = (size_t)hash(entry.first) & mask;
                bool present = false;
                while (slots[i].used){
                    if (slots[i].key == entry.first){
                        present = true;
                        break;
                    }
                    i = (i + 1) & mask;
                }
                if (!present)
                    slots[i] = Slot{entry.first, entry.second, true};
            }
        }

        uint32_t find(std::string_view key) const {
            if (slots.empty())
                return npos;
            size_t i = (size_t)hash(key) & mask;
            while (slots[i].used){
                if (slots[i].key == key)
                    return slots[i].value;
                i = (i + 1) & mask;
            }
            return npos;
        }

        bool contains(std::string_view key) const { return find(key) != npos; }

        static uint64_t hash(std::string_view key){
            uint64_t h = 1469598103934665603ull;
            for (const char c : key){
                h ^= (unsigned char)c;
                h *= 1099511628211ull;
            }
            return h ^ (h >> 29);
        }

    private:
        struct Slot {
            std::string_view key;
            uint32_t value = 0;
            bool used = false;
        };
        std::vector<Slot> slots;
        size_t mask = 0;
    };

    // a set of names (the values are their positions in the list)
    inline StringIndex makeStringSet(std::initializer_list<std::string_view> names){
        std::vector<std::pair<std::string_view, uint32_t>> entries;
        uint32_t i = 0;
        for (std::string_view name : names)
            entries.emplace_back(name, i++);
        return StringIndex(entries);
    }

}

#endif //COMPAT_STRINGINDEX_H
