// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/ui/properties/searchIndex.ts (the match quality, the query with its
// per-word synonyms and phrase, the tiers of rankHit) by SafEight, used with the author's
// permission. The location-only tier, the fuzzy order within a tier and the offers' Japanese
// names are JasperEngine's.

#include "TitlePropertyIndex.h"

#include "TitleSearchFold.h"

#include "search/SearchScorer.h"

#include <algorithm>

using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    using FoldedLabels = PropertyIndex::FoldedLabels;

    constexpr int NONE = -1;
    // a quality step outweighs any fuzzyScore (at most 1200)
    constexpr int QUALITY_WEIGHT = 10000;
    constexpr char LABEL_SEPARATOR = '\x1f';

    // How well `text` holds `word`: 0 equal, 1 starts with it, 2 a word starts with it, 3 holds
    // it, NONE not at all (folded strings: words are separated by single spaces)
    int quality(std::string_view text, std::string_view word) {
        if (word.empty()) return NONE;
        size_t at = text.find(word);
        if (at == std::string_view::npos) return NONE;
        if (at == 0) return text.size() == word.size() ? 0 : 1;
        for (; at != std::string_view::npos; at = text.find(word, at + 1)) {
            if (text[at - 1] == ' ') return 2;
        }
        return 3;
    }

    // keeps the better (smaller) of two qualities
    int better(int a, int b) {
        if (a == NONE) return b;
        if (b == NONE) return a;
        return std::min(a, b);
    }

    // the best quality of `word` in any label
    int inLabels(const FoldedLabels& labels, std::string_view word) {
        // most entries hold none of the words: one scan of the joined texts says so
        if (word.empty() || labels.joined.find(word) == std::string::npos) return NONE;
        int result = NONE;
        for (const std::string& text : labels.texts) result = better(result, quality(text, word));
        return result;
    }

    // the best quality of any of `words` in any label
    int inLabels(const FoldedLabels& labels, const std::vector<std::string>& words) {
        int result = NONE;
        for (const std::string& word : words) {
            result = better(result, inLabels(labels, word));
            if (result == 0) break;
        }
        return result;
    }

    // the best quality of any of `words` in one text
    int inText(std::string_view text, const std::vector<std::string>& words) {
        int result = NONE;
        for (const std::string& word : words) result = better(result, quality(text, word));
        return result;
    }

    // any of `words` anywhere in the location (Pachislop's inPath)
    bool inLocation(std::string_view location, const std::vector<std::string>& words) {
        for (const std::string& word : words) {
            if (!word.empty() && location.find(word) != std::string_view::npos) return true;
        }
        return false;
    }

    // the smallest quality that is not NONE (NONE when all are)
    int minOf(const std::vector<int>& qualities) {
        int result = NONE;
        for (int q : qualities) result = better(result, q);
        return result;
    }

    bool anyMatched(const std::vector<int>& qualities) {
        return std::any_of(qualities.begin(), qualities.end(), [](int q) { return q != NONE; });
    }

    bool allMatched(const std::vector<int>& qualities) {
        return std::none_of(qualities.begin(), qualities.end(), [](int q) { return q == NONE; });
    }

    struct ParsedQuery {
        std::string text;                           // folded
        std::vector<std::string> words;
        std::vector<std::vector<std::string>> alts; // per word: the word and its synonyms
        std::vector<std::string> phrase;            // synonyms of the whole query when it is a group's phrase
    };

    bool parseQuery(std::string_view raw, const SearchTerms& terms, ParsedQuery& q) {
        q.text = foldForSearch(raw);
        if (q.text.empty()) return false;
        size_t at = 0;
        while (at < q.text.size()) {
            const size_t end = std::min(q.text.find(' ', at), q.text.size());
            q.words.push_back(q.text.substr(at, end - at));
            at = end + 1;
        }
        for (const std::string& word : q.words) {
            std::vector<std::string> alts{word};
            for (const std::string& synonym : terms.synonymsOf(word)) alts.push_back(synonym);
            q.alts.push_back(std::move(alts));
        }
        if (q.words.size() > 1) q.phrase = terms.synonymsOf(q.text);
        return true;
    }

    // the best fuzzyScore of the query against any label (0 when none scores)
    int fuzzyAgainst(const ParsedQuery& q, const FoldedLabels& labels) {
        int result = 0;
        for (const std::string& text : labels.texts) result = std::max(result, search::fuzzyScore(q.text, text));
        return result;
    }

    int scoreOf(int quality, int fuzzy) {
        return (3 - std::clamp(quality, 0, 3)) * QUALITY_WEIGHT + fuzzy;
    }

    template <class Hit>
    void order(std::vector<Hit>& hits, size_t limit) {
        // stable: equal tiers and scores keep insertion order
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            return a.tier != b.tier ? a.tier < b.tier : a.score > b.score;
        });
        if (limit > 0 && hits.size() > limit) hits.resize(limit);
    }

    void addFolded(FoldedLabels& labels, std::string_view text) {
        std::string folded = foldForSearch(text);
        if (folded.empty() || std::find(labels.texts.begin(), labels.texts.end(), folded) != labels.texts.end()) return;
        if (!labels.joined.empty()) labels.joined.push_back(LABEL_SEPARATOR);
        labels.joined += folded;
        labels.texts.push_back(std::move(folded));
    }

    // The tier and quality of a hit (tier NONE: no match)
    struct Rank {
        int tier = NONE;
        int quality = 0;
    };

    // The label tiers rows and offers share: the whole query (equal, prefix, word, inside), then
    // every word. Fills `direct` (per word) when the whole query is not in a label.
    Rank rankLabels(const ParsedQuery& q, const FoldedLabels& labels, std::vector<int>& direct) {
        const int whole = inLabels(labels, q.text);
        if (whole != NONE) return {whole, whole};
        direct.clear();
        for (const std::string& word : q.words) direct.push_back(inLabels(labels, word));
        if (allMatched(direct)) return {searchtier::ALL_WORDS, minOf(direct)};
        return {};
    }

}

PropertyIndex::PropertyIndex() : terms(&SearchTerms::builtin()) {}

PropertyIndex::PropertyIndex(const SearchTerms& searchTerms) : terms(&searchTerms) {}

void PropertyIndex::clear() {
    entries.clear();
    folded.clear();
    offers.clear();
    offerLabels.clear();
}

void PropertyIndex::add(IndexEntry entry) {
    FoldedEntry f;
    addFolded(f.labels, entry.label);
    for (const std::string& alias : entry.aliases) addFolded(f.labels, alias);
    for (const std::string& section : entry.location) {
        const std::string part = foldForSearch(section);
        if (part.empty()) continue;
        if (!f.location.empty()) f.location.push_back(' ');
        f.location += part;
    }
    for (const std::string& value : entry.enumLabels) f.enums.push_back(foldForSearch(value));
    entries.push_back(std::move(entry));
    folded.push_back(std::move(f));
}

void PropertyIndex::addOffer(AddOffer offer) {
    FoldedLabels labels;
    addFolded(labels, offer.label);
    for (const std::string& alias : offer.aliases) addFolded(labels, alias);
    addFolded(labels, terms->japaneseName(offer.type));
    offers.push_back(std::move(offer));
    offerLabels.push_back(std::move(labels));
}

std::vector<SearchHit> PropertyIndex::query(std::string_view text, size_t limit) const {
    std::vector<SearchHit> hits;
    ParsedQuery q;
    if (!parseQuery(text, *terms, q)) return hits;
    const size_t wordCount = q.words.size();

    // per-query scratch: no allocation per entry
    std::vector<int> direct, syn, inValue;
    direct.reserve(wordCount);
    syn.reserve(wordCount);
    inValue.reserve(wordCount);

    for (size_t e = 0; e < entries.size(); e++) {
        const FoldedEntry& f = folded[e];
        Rank rank = rankLabels(q, f.labels, direct);
        std::string matchedEnum;
        int fuzzy = NONE;   // against the labels unless the tier sets it

        if (rank.tier == NONE && anyMatched(direct)) {
            // the words over the labels and the location, one in a label ("rim width": Rim 1 › Width)
            bool spread = true;
            for (size_t i = 0; spread && i < wordCount; i++) {
                spread = direct[i] != NONE || f.location.find(q.words[i]) != std::string::npos;
            }
            if (spread) rank = {searchtier::SPREAD, minOf(direct)};
        }
        syn.clear();
        if (rank.tier == NONE) {
            for (const std::vector<std::string>& alts : q.alts) syn.push_back(inLabels(f.labels, alts));
            bool viaSynonyms = anyMatched(syn);
            for (size_t i = 0; viaSynonyms && i < wordCount; i++) {
                viaSynonyms = syn[i] != NONE || inLocation(f.location, q.alts[i]);
            }
            if (viaSynonyms) {
                rank = {searchtier::SYNONYM, minOf(syn)};
            } else if (!q.phrase.empty()) {
                const int phrase = inLabels(f.labels, q.phrase);
                if (phrase != NONE) rank = {searchtier::SYNONYM, phrase};
            }
        }
        if (rank.tier == NONE && !f.location.empty()) {
            // every word (or a synonym) starts a word of the location: the rows of a section
            // ("フチ": the rows of Rim 1 and Rim 2)
            int worst = 0;
            bool all = true;
            for (size_t i = 0; all && i < wordCount; i++) {
                const int at = inText(f.location, q.alts[i]);
                all = at != NONE && at <= 2;
                worst = std::max(worst, at);
            }
            if (!all && !q.phrase.empty()) {
                worst = inText(f.location, q.phrase);
                all = worst != NONE && worst <= 2;
            }
            if (all) rank = {searchtier::LOCATION, worst};
        }
        if (rank.tier == NONE) {
            // an enum value ("Ring": Gloss = Ring), the other words in the labels or the location
            for (size_t v = 0; v < f.enums.size(); v++) {
                inValue.clear();
                for (size_t i = 0; i < wordCount; i++) inValue.push_back(inText(f.enums[v], q.alts[i]));
                bool match = anyMatched(inValue);
                for (size_t i = 0; match && i < wordCount; i++) {
                    match = inValue[i] != NONE || syn[i] != NONE || inLocation(f.location, q.alts[i]);
                }
                if (match) {
                    rank = {searchtier::ENUM_VALUE, minOf(inValue)};
                    matchedEnum = entries[e].enumLabels[v];
                    fuzzy = std::max(0, search::fuzzyScore(q.text, f.enums[v]));
                    break;
                }
            }
        }
        if (rank.tier == NONE) continue;
        if (fuzzy == NONE) fuzzy = fuzzyAgainst(q, f.labels);
        hits.push_back({e, rank.tier, scoreOf(rank.quality, fuzzy), std::move(matchedEnum)});
    }
    order(hits, limit);
    return hits;
}

std::vector<OfferHit> PropertyIndex::queryOffers(std::string_view text, size_t limit) const {
    std::vector<OfferHit> hits;
    ParsedQuery q;
    if (!parseQuery(text, *terms, q)) return hits;

    std::vector<int> direct, syn;
    for (size_t o = 0; o < offers.size(); o++) {
        const FoldedLabels& labels = offerLabels[o];
        Rank rank = rankLabels(q, labels, direct);
        if (rank.tier == NONE) {
            // every word, or one of its synonyms, in the labels ("光彩" finds Outer Glow by "glow")
            syn.clear();
            for (const std::vector<std::string>& alts : q.alts) syn.push_back(inLabels(labels, alts));
            if (allMatched(syn)) {
                int worst = 0;
                for (int x : syn) worst = std::max(worst, x);
                rank = {searchtier::SYNONYM, worst};
            } else if (!q.phrase.empty()) {
                const int phrase = inLabels(labels, q.phrase);
                if (phrase != NONE) rank = {searchtier::SYNONYM, phrase};
            }
        }
        if (rank.tier == NONE) continue;
        hits.push_back({o, rank.tier, scoreOf(rank.quality, fuzzyAgainst(q, labels))});
    }
    order(hits, limit);
    return hits;
}
