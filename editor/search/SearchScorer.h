// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The scorer is ported from OpenEngine (Apps/Editor/Source/UI/UniversalSearchProvider.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#pragma once

// One fuzzy scorer for every search of the editor: the Search palette (Ctrl+K), Add Component,
// the Hierarchy and Project filters and the settings search (docs/specs/S25 §6.1).
//
// Strings are compared after normalize(): lower case, camelCase and acronyms split into words,
// anything that is not a letter or a digit turned into one space ("RigidBody2D" -> "rigid body2 d").
// Letters of other scripts are kept whole as UTF-8 ("激熱 Glow" -> "激熱 glow", S82 R27), so
// Japanese names are searchable in every filter; Unicode punctuation separates words as ASCII
// punctuation does. Typo tolerance (Damerau) applies to ASCII words only: a byte distance between
// two Japanese words means nothing.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor::search {

    // OpenEngine's NormalizeForSearch, with UTF-8 letters kept
    std::string normalize(std::string_view text);

    // What decodeUtf8 returns for a byte that does not start a valid UTF-8 sequence (length 1)
    constexpr char32_t INVALID_UTF8 = 0x110000;
    // The code point of the UTF-8 sequence at `at` (< text.size()) and its length in bytes
    char32_t decodeUtf8(std::string_view text, size_t at, size_t& length);
    // Spaces, punctuation and symbols, ASCII and Unicode (general punctuation, arrows, shapes,
    // CJK and full-width punctuation such as 、。「」・): what normalize() turns into a space.
    // Letters of every script, digits, ideographs, kana (and ー) and INVALID_UTF8 are not.
    bool isSeparator(char32_t codePoint);

    // Optimal-string-alignment Damerau-Levenshtein distance; cutoff + 1 once it is exceeded
    int damerau(std::string_view a, std::string_view b, int cutoff);

    // Ordered subsequence score of q in c (word starts and runs score more), -1 without one
    int subsequence(std::string_view q, std::string_view c);

    // Ranked score of a normalized query in a normalized candidate, -1 when it does not match.
    // Tiers: 1200 exact, 1000 prefix, 800 substring, then per query word the best of word prefix
    // (180), Damerau distance (140 - 35 per edit) and subsequence; a query word that matches no
    // word scores against the whole candidate as one subsequence ("gmo" -> "game object").
    int fuzzyScore(std::string_view q, std::string_view c);

    enum class MatchMode { Rank, Filter };

    // Tree filters (Hierarchy, Project, settings rows) show every included row in tree order, so
    // a bare subsequence hit ("pl" in "spot light") would flood them. Filter accepts a candidate
    // when the normalized query is a substring of it, or when every query word is a word prefix
    // of some candidate word or, from three letters on, within the Damerau cutoff of one that
    // starts with the same letter.
    bool filterMatch(std::string_view normQuery, std::string_view normCandidate);

    // matches(): fuzzyScore() >= 0 in Rank mode, filterMatch() in Filter mode
    bool matches(MatchMode mode, std::string_view normQuery, std::string_view normCandidate);

    // Byte ranges [begin, end) of `label` (not normalized) to highlight for the raw query: each
    // query word where it occurs (case-insensitively, word starts first), else its letters as a
    // subsequence. Sorted and merged.
    std::vector<std::pair<size_t, size_t>> highlightRanges(std::string_view query, std::string_view label);

}
