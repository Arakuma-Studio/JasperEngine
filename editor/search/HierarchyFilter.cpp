// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HierarchyFilter.h"

#include "SearchScorer.h"

#include <cctype>

using namespace jasperengine::editor;

HierarchyFilter::HierarchyFilter(std::string_view query, bool caseSensitive) : matchCase(caseSensitive) {
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (word.size() > 2 && (word[0] == 't' || word[0] == 'T') && word[1] == ':') {
            const std::string type = search::normalize(word.substr(2));
            if (!type.empty()) types.push_back(type);
        } else {
            text += (text.empty() ? "" : " ") + word;
        }
        word.clear();
    };
    for (char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) flush();
        else word.push_back(c);
    }
    flush();
    normalizedText = search::normalize(text);
}

bool HierarchyFilter::matches(std::string_view name, const std::function<std::vector<std::string>()>& components) const {
    if (!text.empty()) {
        if (matchCase) {
            if (name.find(text) == std::string_view::npos) return false;
        } else if (!search::filterMatch(normalizedText, search::normalize(name))) {
            return false;
        }
    }
    if (types.empty()) return true;
    for (const std::string& component : components()) {
        const std::string normalized = search::normalize(component);
        for (const std::string& type : types) {
            if (search::filterMatch(type, normalized)) return true;
        }
    }
    return false;
}
