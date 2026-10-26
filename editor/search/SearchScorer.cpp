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

    bool isUpper(unsigned char c) { return std::isupper(c) != 0; }
    bool isLower(unsigned char c) { return std::islower(c) != 0; }
    bool isDigit(unsigned char c) { return std::isdigit(c) != 0; }

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

std::string search::normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    bool previousSpace = true;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool upper = isUpper(c);
        const bool previousLowerOrDigit = i > 0 &&
            (isLower(static_cast<unsigned char>(text[i - 1])) || isDigit(static_cast<unsigned char>(text[i - 1])));
        const bool acronymBoundary = upper && i > 0 && i + 1 < text.size() &&
            isUpper(static_cast<unsigned char>(text[i - 1])) && isLower(static_cast<unsigned char>(text[i + 1]));
        if ((upper && previousLowerOrDigit) || acronymBoundary) {
            if (!previousSpace) result.push_back(' ');
            previousSpace = true;
        }
        if (std::isalnum(c) != 0) {
            result.push_back(static_cast<char>(std::tolower(c)));
            previousSpace = false;
        } else if (!previousSpace) {
            result.push_back(' ');
            previousSpace = true;
        }
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
    for (size_t ci = 0; ci < c.size() && qi < q.size(); ++ci) {
        if (q[qi] != c[ci]) {
            run = 0;
            continue;
        }
        const bool boundary = ci == 0 || c[ci - 1] == ' ';
        score += 7 + (boundary ? 10 : 0) + std::min(run * 4, 16);
        ++run;
        ++qi;
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

        int best = -1;
        size_t candidateAt = 0;
        while (candidateAt < c.size()) {
            const std::string_view candidateWord = nextWord(c, candidateAt);
            if (candidateWord.empty()) continue;
            if (candidateWord.starts_with(queryWord)) {
                best = std::max(best, 180 - static_cast<int>(candidateWord.size() - queryWord.size()));
            }
            const int cutoff = cutoffFor(queryWord);
            const int distance = damerau(queryWord, candidateWord, cutoff);
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
            } else if (queryWord.size() >= 3) {
                // typos from three letters on: one letter is within one edit of every word
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
