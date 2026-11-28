// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_STRINGTABLERUNTIME_H
#define LOCALIZATION_STRINGTABLERUNTIME_H

// Contract (docs/specs/game-ui-localization.md R1, R2, R13, R26, D5)
// - One language of one namespace, immutable once made: a String Table's source texts (a
//   .stringtable), one translation (a .translation), or the cooked l10n/<ns>.<lang>.strbin an export
//   writes. Keys are the table's own (without the namespace).
// - Memory: the UTF-8 texts in one blob plus 16 bytes per entry (the key's FNV-1a 64 hash, sorted
//   for a binary search, its text's offset, length and flags); keys themselves are not kept.
// - `.strbin`: "JSTB", version, entry count, the hash seed, the language, the sorted hashes, the
//   {offset, length | flags} rows, the blob. A collision of two keys of one table is an export
//   error naming both (cook()).
// - Readers check every offset: a damaged file is refused, never read past.
// - Thread-safety: a made table is read-only, safe from any thread.

#include "Export.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::l10n {

    class JASPERENGINE_API StringTable {
    public:
        struct Entry {
            std::string key;
            std::string text;
            bool needsReview = false;
        };

        static std::shared_ptr<const StringTable> fromEntries(const std::string& language, const std::vector<Entry>& entries,
                                                              uint64_t seed = 0);
        // a .stringtable's source texts (its namespace and source language in `ns`, `language`)
        static std::shared_ptr<const StringTable> fromStringTableJson(std::string_view json, std::string* ns, std::string* language,
                                                                      std::string* error);
        // a .translation's texts (its language in `language`)
        static std::shared_ptr<const StringTable> fromTranslationJson(std::string_view json, std::string* language, std::string* error);
        static std::shared_ptr<const StringTable> fromStrbin(std::string_view data, std::string* error);

        // the .strbin of `entries`; false with an error naming both keys when two hash alike
        static bool cook(const std::string& language, const std::vector<Entry>& entries, std::string& out, std::string* error,
                         uint64_t seed = 0);
        static uint64_t hashKey(std::string_view key, uint64_t seed = 0);

        // the text of `key`; false when the table has none
        bool find(std::string_view key, std::string_view& text, bool* needsReview = nullptr, bool* plain = nullptr) const;
        const std::string& language() const { return lang; }
        size_t size() const { return hashes.size(); }
        // the bytes it holds: texts plus 16 per entry
        size_t memoryBytes() const { return blob.size() + hashes.size() * 16; }

    private:
        static constexpr uint32_t FLAG_PLAIN = 1u << 31;
        static constexpr uint32_t FLAG_REVIEW = 1u << 30;
        static constexpr uint32_t LENGTH_MASK = (1u << 30) - 1;

        std::string lang;
        uint64_t seed = 0;
        std::vector<uint64_t> hashes;
        std::vector<uint32_t> rows;     // per entry: offset, then length | flags
        std::string blob;
    };

}

#endif //LOCALIZATION_STRINGTABLERUNTIME_H
