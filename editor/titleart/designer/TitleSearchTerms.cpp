// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleSearchTerms.h"

#include "TitleSearchFold.h"

#include "Log.h"
#include "json.hpp"

#include "title_search_terms.h"

#include <algorithm>

using namespace jasperengine::editor::titledesigner;

namespace {

    void fail(std::string* error, std::string message) {
        if (error) *error = std::move(message);
    }

}

std::string_view SearchTerms::builtinText() {
    return std::string_view(reinterpret_cast<const char*>(title_search_terms_json), title_search_terms_json_size);
}

const SearchTerms& SearchTerms::builtin() {
    static const SearchTerms terms = []() {
        SearchTerms parsed;
        std::string error;
        if (!parse(builtinText(), parsed, &error)) {
            jasperengine::Log::error("Title Designer: the embedded search-terms.json does not load: %s", error.c_str());
            parsed = SearchTerms();
        }
        return parsed;
    }();
    return terms;
}

bool SearchTerms::parse(std::string_view json, SearchTerms& out, std::string* error) {
    out = SearchTerms();
    const nlohmann::json root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        fail(error, "not a JSON object");
        return false;
    }
    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer() || version->get<int>() != VERSION) {
        fail(error, "version is not " + std::to_string(VERSION));
        return false;
    }
    out.fileVersion = version->get<int>();

    const auto groups = root.find("groups");
    if (groups == root.end() || !groups->is_array()) {
        fail(error, "groups is not an array");
        return false;
    }
    for (const nlohmann::json& group : *groups) {
        if (!group.is_array()) {
            fail(error, "a group is not an array");
            return false;
        }
        std::vector<std::string> terms;
        for (const nlohmann::json& term : group) {
            if (!term.is_string()) {
                fail(error, "a term is not a string");
                return false;
            }
            // "ボカシ" and "ぼかし" fold to one term
            std::string folded = foldForSearch(term.get<std::string>());
            if (!folded.empty() && std::find(terms.begin(), terms.end(), folded) == terms.end()) terms.push_back(std::move(folded));
        }
        if (terms.size() >= 2) out.termGroups.push_back(std::move(terms));
    }
    for (const std::vector<std::string>& group : out.termGroups) {
        for (const std::string& term : group) {
            std::vector<std::string>& others = out.synonyms[term];
            for (const std::string& other : group) {
                if (other != term && std::find(others.begin(), others.end(), other) == others.end()) others.push_back(other);
            }
        }
    }

    const auto names = root.find("names_ja");
    if (names == root.end() || !names->is_object()) {
        fail(error, "names_ja is not an object");
        return false;
    }
    for (auto it = names->begin(); it != names->end(); ++it) {
        if (!it.value().is_string()) {
            fail(error, "names_ja." + it.key() + " is not a string");
            return false;
        }
        out.namesJa[it.key()] = it.value().get<std::string>();
    }
    return true;
}

const std::vector<std::string>& SearchTerms::synonymsOf(std::string_view foldedWord) const {
    static const std::vector<std::string> none;
    const auto it = synonyms.find(foldedWord);
    return it == synonyms.end() ? none : it->second;
}

std::string_view SearchTerms::japaneseName(std::string_view type) const {
    const auto it = namesJa.find(type);
    return it == namesJa.end() ? std::string_view() : std::string_view(it->second);
}
