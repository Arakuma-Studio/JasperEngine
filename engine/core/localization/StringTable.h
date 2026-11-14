// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef STRING_TABLE_H
#define STRING_TABLE_H

// Contract (docs/specs/S95-lua-runtime-gaps.md REQ-25, REQ-37, D-13)
// - Purpose: a String Table (`.stringtable`, Unity's Localization String Table Collection): its
//   keys, and per key a value for each language. JSON version 1, its GUID in its `.meta`:
//     {"version": 1, "entries": [
//       {"key": "menu.play", "values": {"en": "Play", "es": "Jugar"}},
//       {"key": "hud.coins", "comment": "HUD counter", "values": {"en": {"one": "{count} coin", "other": "{count} coins"}}}]}
//   A value is a string, or plural forms (zero, one, two, few, many, other: PluralRules).
// - Keys: not empty, no ':' (Localization's "Table:key"), no surrounding spaces; unique in a
//   table. Languages are BCP 47 tags in canonical case (TitleArtLanguage).
// - parse is lenient per entry (an entry it cannot read is left out and named in `problems`) and
//   strict per file (not JSON, no `entries` list, a newer version: false with `error`). write()
//   gives stable text (entries and languages in their order, two-space indent, a final newline).
// - The CSV / XLIFF interchange writes a plural value as an ICU MessageFormat plural,
//   "{count, plural, one {{count} coin} other {{count} coins}}", which translation tools read.
// - Thread-safety: plain values.

#include "Export.h"
#include "localization/PluralRules.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    struct JASPERENGINE_API StringTableValue{
        bool plural = false;
        std::string text;                                       // the value when not plural
        std::array<std::string, PLURAL_CATEGORY_COUNT> forms;   // by PluralCategory, when plural
        uint8_t present = 0;                                    // a bit per form the value has

        static StringTableValue ofText(const std::string& text);
        bool has(PluralCategory category) const { return (present >> (int)category) & 1u; }
        void setForm(PluralCategory category, const std::string& text);
        // the form of `category`, else `other`, else the first form it has
        const std::string& form(PluralCategory category) const;
        // no text at all (an empty string, or plural forms that are all empty)
        bool empty() const;
        // the interchange form: the text, or an ICU plural over `count`
        std::string toMessageFormat() const;
        // a text in either form; a cell that only starts like a plural stays text
        static StringTableValue fromMessageFormat(const std::string& text);
        bool operator==(const StringTableValue& other) const;
        bool operator!=(const StringTableValue& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API StringTableEntry{
        std::string key;
        std::string comment;                                    // the translators' note
        std::vector<std::pair<std::string, StringTableValue>> values;   // language -> value, in order

        // the value of exactly `language` (case-insensitive), null when none
        const StringTableValue* find(const std::string& language) const;
        // replaces or appends
        void set(const std::string& language, const StringTableValue& value);
        bool erase(const std::string& language);
        bool operator==(const StringTableEntry& other) const;
    };

    struct JASPERENGINE_API StringTable{
        static constexpr int VERSION = 1;
        static constexpr const char* EXTENSION = ".stringtable";

        std::vector<StringTableEntry> entries;

        const StringTableEntry* find(const std::string& key) const;
        StringTableEntry* find(const std::string& key);
        bool erase(const std::string& key);
        // every language with a value, in first-seen order
        std::vector<std::string> languages() const;
        // a copy keeping the values of `languages` only (an export's project languages); empty keeps all
        StringTable onlyLanguages(const std::vector<std::string>& languages) const;

        static bool parse(const std::string& text, StringTable& out, std::string* error, std::vector<std::string>* problems = nullptr);
        std::string write() const;

        // why `key` cannot be a key ("" when it can)
        static std::string keyProblem(const std::string& key);
        // the name "Table:key" uses: the file name without its extension ("ui/Menu.stringtable" -> "Menu")
        static std::string nameOfPath(const std::string& path);
        bool operator==(const StringTable& other) const { return entries == other.entries; }
    };

}

#endif //STRING_TABLE_H
