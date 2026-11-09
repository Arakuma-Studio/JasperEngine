// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R34, §6.13)
// - Purpose: the library's search, shared by the Title Designer's Library panel (S82) and
//   list_title_library. Every word of the query must match one of an item's fields: its English and
//   Japanese names, its synonyms (both languages, romaji included), tags, family names (both
//   languages and the id), dimension, and the derived terms of what it uses (§6.13: "light glow" /
//   ライトグロー for light_glow, "hollow" / 中抜き for a knockout, "zebra" / ゼブラ ...). Queries and
//   fields are folded with S82's foldForSearch (full-width to ASCII, katakana to hiragana, lower
//   case), so ＧＯＬＤ, gold, ゴールド and ごーるど meet the same items.
// - A word matches a field when the field holds it (a substring), a field word starts with it, or
//   (ASCII words of 5 letters or more) one edit away from a field word. Results rank by S25's
//   fuzzyScore, names first, synonyms and tags 80 lower, then by name.
// - Never uses the network.
// - Thread-safety: main thread (the library store).

#pragma once

#include "TitleLibrary.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    struct LibraryQuery {
        std::string text;
        std::optional<LibraryKind> kind;
        std::string family;
        std::string dimension;                  // "2d", "3d"
        std::vector<std::string> tags;          // all must be present
        std::optional<LibraryScope> scope;
        bool favoritesOnly = false;
        std::vector<std::string> favorites;     // the refs starred (favoritesOnly)
        bool availableOnly = false;
    };

    struct LibraryHit {
        const LibraryItemInfo* item = nullptr;
        int score = 0;
    };

    // the matching items of every kind the query allows, best first
    std::vector<LibraryHit> searchLibrary(const LibraryQuery& query);
    // the derived terms of an item in English and Japanese (§6.13), unfolded
    std::vector<std::string> derivedTermWords(const LibraryItemInfo& item);
    // every searchable text of an item (S82's tile `terms`)
    std::vector<std::string> searchFields(const LibraryItemInfo& item);

}
