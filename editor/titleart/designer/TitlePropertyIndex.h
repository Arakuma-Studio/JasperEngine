// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R26, D4)
// - Purpose: the storage and the query of the Properties search. The collect pass (the page
//   functions run in RowMode::Collect) adds one IndexEntry per row it would draw, and the Add
//   palette one AddOffer per effect or generator the layer could get; query() ranks the rows for
//   what the person typed, queryOffers() the offers ("+ Add Outer Glow").
// - Matching, in tiers (lower is better): the whole query in a label or alias (equal, prefix,
//   word start, inside), every word in the labels, the words spread over the labels and the
//   location with at least one in a label ("rim width" finds Rim 1 › Width), synonyms of the words
//   (search-terms.json, SearchTerms) in the labels or the location, every word (or a synonym) at a
//   word start of the location only ("フチ" finds the rows of Rim 1), an enum value label ("Ring"
//   finds Gloss = Ring). Within a tier the best quality of the matched words, then
//   search::fuzzyScore against the label; equal scores keep insertion order (stable keys of the
//   live results). Query and texts are folded with foldForSearch, so English and Japanese,
//   full-width and half-width, katakana and hiragana all meet. Offers match their label, aliases
//   and Japanese name (names_ja) in the label tiers and through synonyms.
// - Cost: the folded texts are computed once in add(); a query over ~400 entries does a few
//   substring scans per entry and allocates only its result (well under 1 ms).
// - Thread-safety: none; the Title Designer's main thread owns it. The SearchTerms it reads must
//   outlive it (builtin() lives for the process).

#pragma once

#include "TitleDesignerTypes.h"
#include "TitleSearchTerms.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titledesigner {

    // One row the collect pass found
    struct IndexEntry {
        std::string rowId;                    // stable key (node + field path), the hit's identity
        std::string label;                    // "Width"
        std::vector<std::string> aliases;     // extra words: an effect's registry aliases, a Japanese label
        std::vector<std::string> location;    // section chain shown above the hit: {"Rim 2", "Bevel"}
        NodeRef node;                         // the node whose page holds the row
        std::string nodeLabel;                // "Title" (the layer's name), for "Search every layer" grouping
        std::vector<FieldRef> targets;        // what the live control edits
        std::string fieldPath;                // "style.rims[1].width" (tooltip, Copy Property Path)
        std::string kind;                     // "number", "integer", "bool", "enum", "color", "gradient", "curve", "asset", "text", "vec2"
        std::vector<std::string> enumLabels;  // labels of enum values ("Ring" finds Gloss = Ring)
        bool link = false;                    // works only on its page: shown as a link
    };

    // An effect or generator the selection does not have, offered below the hits
    struct AddOffer {
        std::string kind;                     // "effect" | "generator"
        std::string type;                     // registry id ("outer_glow"): its names_ja entry is searched too
        std::string label;                    // "Outer Glow"
        std::vector<std::string> aliases;     // the registry's aliases
        std::string category;                 // the Add palette's group ("Glow")
    };

    // The tiers of a hit (SearchHit::tier, OfferHit::tier), best first
    namespace searchtier {
        constexpr int LABEL_EQUAL = 0;        // the whole query is a label
        constexpr int LABEL_PREFIX = 1;       // a label starts with it
        constexpr int LABEL_WORD = 2;         // a word of a label starts with it
        constexpr int LABEL_INSIDE = 3;       // a label holds it
        constexpr int ALL_WORDS = 4;          // every word in the labels
        constexpr int SPREAD = 5;             // the words over labels and location, one in a label
        constexpr int SYNONYM = 6;            // as SPREAD with synonyms, or the phrase's synonyms
        constexpr int LOCATION = 7;           // every word (or a synonym) at a word start of the location
        constexpr int ENUM_VALUE = 8;         // an enum value label
    }

    struct SearchHit {
        size_t entry = 0;                     // index into the PropertyIndex
        int tier = 0;                         // searchtier
        int score = 0;                        // within the tier, higher first
        std::string matchedEnumLabel;         // ENUM_VALUE: the enum value label as given ("Ring")
    };

    struct OfferHit {
        size_t offer = 0;
        int tier = 0;
        int score = 0;
    };

    class PropertyIndex {
    public:
        // the embedded search terms (SearchTerms::builtin())
        PropertyIndex();
        explicit PropertyIndex(const SearchTerms& terms);

        void clear();                          // entries and offers
        void add(IndexEntry entry);
        void addOffer(AddOffer offer);

        size_t size() const { return entries.size(); }
        size_t offerCount() const { return offers.size(); }
        const IndexEntry& entry(size_t i) const { return entries[i]; }
        const AddOffer& offer(size_t i) const { return offers[i]; }

        // the matching entries, best first, at most `limit` (0: all); empty for an empty query
        std::vector<SearchHit> query(std::string_view text, size_t limit) const;
        std::vector<OfferHit> queryOffers(std::string_view text, size_t limit) const;

        // the folded texts a label tier searches (public for the .cpp's helpers)
        struct FoldedLabels {
            std::vector<std::string> texts;    // folded, without duplicates
            std::string joined;                // texts joined by '\x1f' (never in a folded text): one scan rejects a word
        };

    private:
        struct FoldedEntry {
            FoldedLabels labels;               // label and aliases
            std::string location;              // the location chain folded, joined by spaces
            std::vector<std::string> enums;    // the enum value labels, folded
        };

        const SearchTerms* terms = nullptr;
        std::vector<IndexEntry> entries;
        std::vector<FoldedEntry> folded;
        std::vector<AddOffer> offers;
        std::vector<FoldedLabels> offerLabels; // label, aliases and Japanese name
    };

}
