// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R26, R85)
// - Purpose: the search vocabulary of the Title Designer: synonym groups (English and Japanese:
//   "フチ" finds rims, "ぼかし" finds blur) and one Japanese name per effect and generator type,
//   read from `editor/resources/titleart/search-terms.json` (embedded in the editor, never
//   shipped to players).
// - Design: `{"version": 1, "groups": [[term...]...], "names_ja": {"<type>": "..."}}`. Every term
//   is stored folded (foldForSearch), so a query word is looked up after the same fold. A term in
//   several groups has the union of their terms as synonyms; a group's relation is symmetric.
//   builtin() parses the embedded file once, on first use.
// - Thread-safety: builtin() is a function-local static (thread-safe initialisation); a parsed
//   SearchTerms is immutable and can be read from any thread.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titledesigner {

    class SearchTerms {
    public:
        static constexpr int VERSION = 1;

        // the embedded search-terms.json (empty, with the error logged, if it fails to parse)
        static const SearchTerms& builtin();
        // the embedded file's text
        static std::string_view builtinText();

        // false with `error` on malformed JSON, a version other than VERSION or members of the
        // wrong type
        static bool parse(std::string_view json, SearchTerms& out, std::string* error = nullptr);

        int version() const { return fileVersion; }
        // the groups, folded, each without duplicates
        const std::vector<std::vector<std::string>>& groups() const { return termGroups; }
        // the other terms of every group holding the folded word or phrase (folded, without the
        // word itself); empty when no group holds it
        const std::vector<std::string>& synonymsOf(std::string_view foldedWord) const;
        // the Japanese name of an effect or generator type as written in the file ("" when none)
        std::string_view japaneseName(std::string_view type) const;
        const std::map<std::string, std::string, std::less<>>& japaneseNames() const { return namesJa; }

    private:
        int fileVersion = 0;
        std::vector<std::vector<std::string>> termGroups;
        std::map<std::string, std::vector<std::string>, std::less<>> synonyms;
        std::map<std::string, std::string, std::less<>> namesJa;
    };

}
