// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// normalize(), damerau(), subsequence() and the tiers of fuzzyScore() are ported from OpenEngine
// (Apps/Editor/Source/UI/UniversalSearchProvider.cpp: NormalizeForSearch, DamerauDistance,
// SubsequenceScore, FuzzyScore), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License (NOTICE.md). The whole-candidate subsequence fallback, filterMatch() and
// highlightRanges() are JasperEngine's.

#include "SearchScorer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine::editor;

namespace {

    // ASCII only, whatever the C locale: a byte of a UTF-8 sequence is never a case or digit
    // boundary, so the camelCase split cannot cut a multi-byte character
    bool isUpper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
    bool isLower(unsigned char c) { return c >= 'a' && c <= 'z'; }
    bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
    bool isAsciiLetterOrDigit(unsigned char c) { return isUpper(c) || isLower(c) || isDigit(c); }

    bool isAscii(std::string_view word) {
        for (char c : word) {
            if (static_cast<unsigned char>(c) >= 0x80) return false;
        }
        return true;
    }

    bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

    // The bytes of the character starting at `at` (1 for ASCII and for a stray byte)
    size_t characterLength(std::string_view text, size_t at) {
        size_t length = 1;
        search::decodeUtf8(text, at, length);
        return length;
    }

    // The next space-separated word of a normalized string from `at` (advanced past it)
    std::string_view nextWord(std::string_view text, size_t& at) {
        while (at < text.size() && text[at] == ' ') at++;
        const size_t begin = at;
        while (at < text.size() && text[at] != ' ') at++;
        return text.substr(begin, at - begin);
    }

    // The Damerau cutoff of a query word (OpenEngine's)
    int cutoffFor(std::string_view queryWord) {
        return queryWord.size() <= 5 ? 1 : 2;
    }

    std::string withoutSpaces(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c != ' ') out.push_back(c);
        }
        return out;
    }

    char lowerChar(char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

}

char32_t search::decodeUtf8(std::string_view text, size_t at, size_t& length) {
    length = 1;
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) return lead;
    size_t count = 0;
    char32_t codePoint = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        count = 1;
        codePoint = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        count = 2;
        codePoint = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        count = 3;
        codePoint = lead & 0x07;
        minimum = 0x10000;
    } else {
        return INVALID_UTF8;
    }
    if (at + count >= text.size()) return INVALID_UTF8;
    for (size_t k = 1; k <= count; k++) {
        const unsigned char next = static_cast<unsigned char>(text[at + k]);
        if (!isContinuation(next)) return INVALID_UTF8;
        codePoint = (codePoint << 6) | (next & 0x3F);
    }
    // overlong forms, surrogates and values past U+10FFFF are not characters
    if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return INVALID_UTF8;
    length = count + 1;
    return codePoint;
}

bool search::isSeparator(char32_t c) {
    if (c < 0x80) return !isAsciiLetterOrDigit(static_cast<unsigned char>(c));
    if (c == INVALID_UTF8) return false;
    // Latin-1: controls, no-break space, signs and marks; not ª ² ³ µ ¹ º ¼ ½ ¾
    if (c <= 0xBF) return c != 0xAA && c != 0xB2 && c != 0xB3 && c != 0xB5 && c != 0xB9 && c != 0xBA && (c < 0xBC || c > 0xBE);
    if (c == 0xD7 || c == 0xF7) return true;                     // × ÷
    if (c >= 0x2000 && c <= 0x206F) return true;                 // general punctuation and spaces
    if (c >= 0x2190 && c <= 0x23FF) return true;                 // arrows, mathematical and technical symbols
    if (c >= 0x2500 && c <= 0x27BF) return true;                 // box drawing, shapes, symbols, dingbats
    if (c >= 0x2E00 && c <= 0x2E7F) return true;                 // supplemental punctuation
    if (c >= 0x3000 && c <= 0x3004) return true;                 // ideographic space 、 。 〃 〄
    if (c >= 0x3008 && c <= 0x3020) return true;                 // CJK brackets, 〜 and marks
    if (c == 0x3030 || c == 0x30FB) return true;                 // 〰 ・
    if (c >= 0xFE10 && c <= 0xFE1F) return true;                 // vertical forms
    if (c >= 0xFE30 && c <= 0xFE6F) return true;                 // CJK compatibility and small forms
    if (c >= 0xFF01 && c <= 0xFF0F) return true;                 // full-width ! .. /
    if (c >= 0xFF1A && c <= 0xFF20) return true;                 // full-width : .. @
    if (c >= 0xFF3B && c <= 0xFF40) return true;                 // full-width [ .. `
    if (c >= 0xFF5B && c <= 0xFF65) return true;                 // full-width { .. ~, half-width ｡ ｢ ｣ ､ ･
    return c == 0xFEFF || c == 0xFFFD;
}

std::string search::normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    bool previousSpace = true;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            // a UTF-8 character stays whole (Japanese names are words too, S82 R27); its
            // punctuation separates words as ASCII punctuation does
            size_t length = 1;
            if (isSeparator(decodeUtf8(text, i, length))) {
                if (!previousSpace) result.push_back(' ');
                previousSpace = true;
            } else {
                result.append(text.substr(i, length));
                previousSpace = false;
            }
            i += length;
            continue;
        }
        const bool upper = isUpper(c);
        const bool previousLowerOrDigit = i > 0 &&
            (isLower(static_cast<unsigned char>(text[i - 1])) || isDigit(static_cast<unsigned char>(text[i - 1])));
        const bool acronymBoundary = upper && i > 0 && i + 1 < text.size() &&
            isUpper(static_cast<unsigned char>(text[i - 1])) && isLower(static_cast<unsigned char>(text[i + 1]));
        if ((upper && previousLowerOrDigit) || acronymBoundary) {
            if (!previousSpace) result.push_back(' ');
            previousSpace = true;
        }
        if (isAsciiLetterOrDigit(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
            previousSpace = false;
        } else if (!previousSpace) {
            result.push_back(' ');
            previousSpace = true;
        }
        ++i;
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

int search::damerau(std::string_view a, std::string_view b, int cutoff) {
    // Optimal-string-alignment Damerau-Levenshtein distance: an adjacent letter inversion is
    // one edit, so fast typing such as "brnach" still resolves to "branch"
    if (std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size())) > cutoff) return cutoff + 1;
    // runs for every query word / candidate word pair: per-thread scratch rows, so a keystroke
    // makes no heap allocations
    thread_local std::vector<int> previous;
    thread_local std::vector<int> current;
    thread_local std::vector<int> beforePrevious;
    previous.resize(b.size() + 1);
    current.resize(b.size() + 1);
    beforePrevious.resize(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) previous[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        int rowBest = current[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                current[j] = std::min(current[j], beforePrevious[j - 2] + 1);
            }
            rowBest = std::min(rowBest, current[j]);
        }
        if (rowBest > cutoff) return cutoff + 1;
        beforePrevious.swap(previous);
        previous.swap(current);
    }
    return previous[b.size()];
}

int search::subsequence(std::string_view q, std::string_view c) {
    size_t qi = 0;
    int score = 0;
    int run = 0;
    for (size_t ci = 0; ci < c.size() && qi < q.size();) {
        // whole characters: the bytes of one UTF-8 character never stand in for another's
        const size_t length = characterLength(q, qi);
        if (isContinuation(static_cast<unsigned char>(c[ci])) || c.substr(ci, length) != q.substr(qi, length)) {
            run = 0;
            ++ci;
            continue;
        }
        const bool boundary = ci == 0 || c[ci - 1] == ' ';
        score += 7 + (boundary ? 10 : 0) + std::min(run * 4, 16);
        ++run;
        qi += length;
        ci += length;
    }
    return qi == q.size() ? score - static_cast<int>(c.size() - q.size()) : -1;
}

int search::fuzzyScore(std::string_view q, std::string_view c) {
    if (q.empty()) return 0;
    if (c.empty()) return -1;
    if (q == c) return 1200;
    if (c.starts_with(q)) return 1000 - static_cast<int>(c.size() - q.size());
    if (const size_t at = c.find(q); at != std::string_view::npos) {
        return 800 - static_cast<int>(at * 4 + c.size() - q.size());
    }

    int total = 0;
    size_t queryAt = 0;
    bool wordsMatched = true;
    while (queryAt < q.size()) {
        const std::string_view queryWord = nextWord(q, queryAt);
        if (queryWord.empty()) continue;
        const bool queryIsAscii = isAscii(queryWord);

        int best = -1;
        size_t candidateAt = 0;
        while (candidateAt < c.size()) {
            const std::string_view candidateWord = nextWord(c, candidateAt);
            if (candidateWord.empty()) continue;
            if (candidateWord.starts_with(queryWord)) {
                best = std::max(best, 180 - static_cast<int>(candidateWord.size() - queryWord.size()));
            }
            const int cutoff = cutoffFor(queryWord);
            const int distance = queryIsAscii ? damerau(queryWord, candidateWord, cutoff) : cutoff + 1;
            if (distance <= cutoff) {
                best = std::max(best, 140 - distance * 35 - std::abs(static_cast<int>(candidateWord.size() - queryWord.size())));
            }
            best = std::max(best, subsequence(queryWord, candidateWord));
        }
        if (best < 0) {
            wordsMatched = false;
            break;
        }
        total += best;
    }
    if (wordsMatched) return total;
    // a query that spans words without spaces ("gmo" for "game object"): one subsequence over
    // the whole candidate, which word starts and runs still score
    return subsequence(withoutSpaces(q), c);
}

bool search::filterMatch(std::string_view normQuery, std::string_view normCandidate) {
    if (normQuery.empty()) return true;
    if (normCandidate.empty()) return false;
    if (normCandidate.find(normQuery) != std::string_view::npos) return true;
    // the words typed together or apart ("rigid body" for "Rigidbody2D", "spotlight" for "Spot Light")
    const std::string compactQuery = withoutSpaces(normQuery);
    if (withoutSpaces(normCandidate).find(compactQuery) != std::string::npos) return true;

    size_t queryAt = 0;
    while (queryAt < normQuery.size()) {
        const std::string_view queryWord = nextWord(normQuery, queryAt);
        if (queryWord.empty()) continue;
        bool found = false;
        size_t candidateAt = 0;
        while (!found && candidateAt < normCandidate.size()) {
            const std::string_view candidateWord = nextWord(normCandidate, candidateAt);
            if (candidateWord.empty()) continue;
            if (candidateWord.starts_with(queryWord)) {
                found = true;
            } else if (queryWord.size() >= 3 && queryWord[0] == candidateWord[0] && isAscii(queryWord)) {
                // typos from three letters on (one letter is within one edit of every word), with
                // the first letter right: "vsync" must not find every "sync"; ASCII words only
                // (two kana a code point apart are one byte apart: "あい" is no typo of "あう")
                const int cutoff = cutoffFor(queryWord);
                found = damerau(queryWord, candidateWord, cutoff) <= cutoff;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool search::matches(MatchMode mode, std::string_view normQuery, std::string_view normCandidate) {
    return mode == MatchMode::Filter ? filterMatch(normQuery, normCandidate) : fuzzyScore(normQuery, normCandidate) >= 0;
}

std::vector<std::pair<size_t, size_t>> search::highlightRanges(std::string_view query, std::string_view label) {
    std::vector<std::pair<size_t, size_t>> ranges;
    std::string lowerLabel(label);
    std::transform(lowerLabel.begin(), lowerLabel.end(), lowerLabel.begin(), lowerChar);

    // the query's words as typed (letters and digits), lower case
    std::vector<std::string> words;
    std::string word;
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            word.push_back(lowerChar(c));
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(std::move(word));

    auto isWordStart = [&](size_t at) {
        if (at == 0) return true;
        const unsigned char before = static_cast<unsigned char>(label[at - 1]);
        const unsigned char here = static_cast<unsigned char>(label[at]);
        return std::isalnum(before) == 0 || (isUpper(here) && (isLower(before) || isDigit(before)));
    };

    for (const std::string& queryWord : words) {
        // the first occurrence at a word start, else the first anywhere
        size_t found = std::string::npos;
        for (size_t at = lowerLabel.find(queryWord); at != std::string::npos; at = lowerLabel.find(queryWord, at + 1)) {
            if (found == std::string::npos) found = at;
            if (isWordStart(at)) {
                found = at;
                break;
            }
        }
        if (found != std::string::npos) {
            ranges.push_back({found, found + queryWord.size()});
            continue;
        }
        // its letters in order (a typo or an abbreviation), word starts preferred by the scan
        size_t qi = 0;
        std::vector<std::pair<size_t, size_t>> letters;
        for (size_t at = 0; at < lowerLabel.size() && qi < queryWord.size(); at++) {
            if (lowerLabel[at] == queryWord[qi]) {
                letters.push_back({at, at + 1});
                qi++;
            }
        }
        if (qi == queryWord.size()) ranges.insert(ranges.end(), letters.begin(), letters.end());
    }

    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<size_t, size_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}
