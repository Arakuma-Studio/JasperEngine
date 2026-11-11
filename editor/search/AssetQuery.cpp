// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetQuery.h"

#include "SearchScorer.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>

using namespace jasperengine::editor;

namespace {

    std::string lower(std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    struct TokenEntry {
        AssetQuery::Matcher matcher;
        std::string description;
    };

    std::mutex& tokenMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::map<std::string, TokenEntry>& tokenTable();

    template <class T>
    T& cached(AssetQuery::Context& context, const std::string& key, const std::function<T()>& make) {
        auto it = context.cache.find(key);
        if (it == context.cache.end()) it = context.cache.emplace(key, std::make_shared<T>(make())).first;
        return *static_cast<T*>(it->second.get());
    }

    void registerBuiltins(std::map<std::string, TokenEntry>& table) {
        table["t"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context&) {
            for (const std::string& type : AssetQuery::typeNamesFor(value)) {
                if (type == record.type) return true;
            }
            return false;
        }, "type: an asset type (texture, model, ...) or a Unity name (Texture2D, AudioClip, Folder...)"};
        table["l"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context&) {
            const std::string wanted = lower(value);
            return std::any_of(record.labels.begin(), record.labels.end(), [&](const std::string& label) { return lower(label) == wanted; });
        }, "label: an asset label (case does not matter)"};
        table["ext"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context&) {
            std::string wanted = lower(value);
            if (!wanted.empty() && wanted.front() != '.') wanted.insert(wanted.begin(), '.');
            return !record.folder && record.ext == wanted;
        }, "extension: png, .gltf"};
        table["path"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context&) {
            return record.pathLower.find(lower(value)) != std::string::npos;
        }, "path: a part of the project-relative path"};
        table["re"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context& context) {
            auto& pattern = cached<std::shared_ptr<std::regex>>(context, "re:" + value, [&value]() {
                try {
                    return std::make_shared<std::regex>(value, std::regex::ECMAScript | std::regex::icase);
                } catch (const std::regex_error&) {
                    return std::shared_ptr<std::regex>();
                }
            });
            return pattern && std::regex_search(record.name, *pattern);
        }, "regular expression (ECMAScript, case-insensitive) on the name"};
        table["ref"] = {[](const AssetRecord& record, const std::string& value, AssetQuery::Context& context) {
            const auto& referrers = cached<std::unordered_set<std::string>>(context, "ref:" + value, [&]() {
                return context.index ? context.index->referrersOf(value) : std::unordered_set<std::string>();
            });
            return (!record.guid.empty() && referrers.count(record.guid)) || referrers.count(record.path);
        }, "assets that reference this path or GUID"};
        table["missing"] = {[](const AssetRecord& record, const std::string&, AssetQuery::Context& context) {
            const auto& missing = cached<std::unordered_set<std::string>>(context, "missing:", [&]() {
                return context.index ? context.index->withMissingReferences() : std::unordered_set<std::string>();
            });
            return !record.guid.empty() && missing.count(record.guid) > 0;
        }, "assets with missing references"};
    }

    std::map<std::string, TokenEntry>& tokenTable() {
        static std::map<std::string, TokenEntry> table = [] {
            std::map<std::string, TokenEntry> built;
            registerBuiltins(built);
            return built;
        }();
        return table;
    }

    // "t:texture" -> key t, value texture; quoted values ("path:\"my folder\"") keep their spaces
    std::vector<std::string> splitTokens(std::string_view text) {
        std::vector<std::string> tokens;
        std::string current;
        bool quoted = false;
        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i];
            if (c == '"') {
                quoted = !quoted;
                current.push_back(c);
                continue;
            }
            if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) tokens.push_back(std::move(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) tokens.push_back(std::move(current));
        return tokens;
    }

    std::string unquote(const std::string& text) {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
        if (!text.empty() && text.front() == '"') return text.substr(1);
        return text;
    }

    std::string quoteIfSpaced(const std::string& value) {
        return value.find(' ') != std::string::npos ? "\"" + value + "\"" : value;
    }

}

void jasperengine::editor::prepareAssetRecord(AssetRecord& record) {
    record.nameLower = lower(record.name);
    record.pathLower = lower(record.path);
    record.nameNormalized = search::normalize(record.name);
    record.pathNormalized = search::normalize(record.path);
}

void AssetQuery::registerToken(const std::string& key, Matcher matcher, std::string description) {
    std::lock_guard<std::mutex> lock(tokenMutex());
    tokenTable()[lower(key)] = {std::move(matcher), std::move(description)};
}

bool AssetQuery::isToken(const std::string& key) {
    std::lock_guard<std::mutex> lock(tokenMutex());
    return tokenTable().count(lower(key)) > 0;
}

std::vector<std::pair<std::string, std::string>> AssetQuery::tokens() {
    std::lock_guard<std::mutex> lock(tokenMutex());
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [key, entry] : tokenTable()) out.push_back({key, entry.description});
    return out;
}

std::vector<std::string> AssetQuery::typeNamesFor(const std::string& value) {
    const std::string key = lower(value);
    static const std::map<std::string, std::vector<std::string>> aliases = {
        {"texture2d", {"texture"}}, {"texture", {"texture"}}, {"sprite", {"texture"}}, {"image", {"texture"}},
        {"material", {"material"}}, {"scene", {"scene"}}, {"sceneasset", {"scene"}},
        {"prefab", {"prefab"}}, {"gameobject", {"prefab"}},
        {"model", {"model"}}, {"mesh", {"model", "sub_asset"}},
        {"audioclip", {"audio"}}, {"audio", {"audio"}},
        {"font", {"font"}}, {"shader", {"shader"}},
        {"script", {"script"}}, {"monoscript", {"script"}},
        {"textasset", {"data", "ui_document", "stylesheet"}},
        {"folder", {"folder"}}, {"defaultasset", {"folder"}},
        {"ui_document", {"ui_document"}}, {"uidocument", {"ui_document"}}, {"html", {"ui_document"}},
        {"stylesheet", {"stylesheet"}}, {"css", {"stylesheet"}},
        {"data", {"data"}}, {"color_lut", {"color_lut"}}, {"lut", {"color_lut"}}, {"sub_asset", {"sub_asset"}},
        {"title_art", {"title_art"}}, {"titleart", {"title_art"}},
    };
    auto it = aliases.find(key);
    return it == aliases.end() ? std::vector<std::string>{} : it->second;
}

AssetQuery AssetQuery::parse(std::string_view text) {
    AssetQuery query;
    query.groupList.emplace_back();
    for (const std::string& token : splitTokens(text)) {
        if (token == "OR") {
            if (!query.groupList.back().terms.empty()) query.groupList.emplace_back();
            continue;
        }
        if (token.size() >= 2 && token.front() == '"') {
            const std::string phrase = unquote(token);
            if (!phrase.empty()) query.groupList.back().terms.push_back({"\"", phrase});
            continue;
        }
        const size_t colon = token.find(':');
        if (colon != std::string::npos && colon > 0) {
            const std::string key = lower(token.substr(0, colon));
            if (isToken(key)) {
                query.groupList.back().terms.push_back({key, unquote(token.substr(colon + 1))});
                continue;
            }
        }
        // unknown keys are bare text
        query.groupList.back().terms.push_back({"", token});
    }
    if (query.groupList.size() > 1 && query.groupList.back().terms.empty()) query.groupList.pop_back();
    query.compile();
    return query;
}

void AssetQuery::compile() {
    compiled.clear();
    errorText.clear();
    for (const Group& group : groupList) {
        Compiled out;
        std::string bare;
        std::map<std::string, std::vector<std::string>> byKey;
        for (const Term& term : group.terms) {
            if (term.key.empty()) {
                bare += (bare.empty() ? "" : " ") + term.value;
            } else if (term.key == "\"") {
                out.phrases.push_back(lower(term.value));
            } else {
                byKey[term.key].push_back(term.value);
                if (term.key == "re") {
                    try {
                        std::regex test(term.value, std::regex::ECMAScript | std::regex::icase);
                    } catch (const std::regex_error& e) {
                        out.regexFailed = true;
                        if (errorText.empty()) errorText = "re:" + term.value + " is not a valid regular expression (" + e.what() + ")";
                    }
                }
            }
        }
        out.bareNormalized = search::normalize(bare);
        {
            std::lock_guard<std::mutex> lock(tokenMutex());
            for (auto& [key, values] : byKey) {
                auto entry = tokenTable().find(key);
                out.keyed.push_back({entry != tokenTable().end() ? entry->second.matcher : Matcher(), std::move(values)});
            }
        }
        compiled.push_back(std::move(out));
    }
}

std::string AssetQuery::toString() const {
    std::string out;
    for (size_t g = 0; g < groupList.size(); g++) {
        if (g > 0) out += " OR ";
        std::string group;
        for (const Term& term : groupList[g].terms) {
            if (!group.empty()) group += ' ';
            if (term.key.empty()) group += term.value;
            else if (term.key == "\"") group += "\"" + term.value + "\"";
            else group += term.key + ":" + quoteIfSpaced(term.value);
        }
        out += group;
    }
    return out;
}

bool AssetQuery::empty() const {
    return std::all_of(groupList.begin(), groupList.end(), [](const Group& group) { return group.terms.empty(); });
}

bool AssetQuery::matches(const AssetRecord& record, Context& context, int* score) const {
    if (score) *score = 0;
    bool any = false;
    int best = -1;
    for (const Compiled& group : compiled) {
        if (group.regexFailed) continue;
        bool ok = true;
        if (!group.bareNormalized.empty() && !search::filterMatch(group.bareNormalized, record.nameNormalized)) ok = false;
        for (size_t i = 0; ok && i < group.phrases.size(); i++) {
            ok = record.nameLower.find(group.phrases[i]) != std::string::npos;
        }
        for (auto it = group.keyed.begin(); ok && it != group.keyed.end(); ++it) {
            const Matcher& matcher = it->first;
            if (!matcher) {
                ok = false;
                break;
            }
            ok = std::any_of(it->second.begin(), it->second.end(), [&](const std::string& value) { return matcher(record, value, context); });
        }
        if (!ok) continue;
        any = true;
        const int groupScore = group.bareNormalized.empty() ? 0 : std::max(0, search::fuzzyScore(group.bareNormalized, record.nameNormalized));
        best = std::max(best, groupScore);
    }
    if (any && score) *score = best;
    return any;
}

std::vector<AssetQuery::Match> AssetQuery::evaluate(const AssetIndexView& index, const std::string& folder) const {
    std::vector<Match> out;
    Context context;
    context.index = &index;
    const std::string prefix = folder.empty() || folder == "." ? std::string() : folder + "/";
    const bool all = empty();
    for (size_t i = 0; i < index.size(); i++) {
        const AssetRecord& record = index.at(i);
        if (!prefix.empty() && record.path.compare(0, prefix.size(), prefix) != 0) continue;
        int score = 0;
        if (!all && !matches(record, context, &score)) continue;
        out.push_back({i, score});
    }
    std::sort(out.begin(), out.end(), [&index](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        const AssetRecord& ra = index.at(a.index);
        const AssetRecord& rb = index.at(b.index);
        if (ra.nameLower != rb.nameLower) return ra.nameLower < rb.nameLower;
        return ra.path < rb.path;
    });
    return out;
}
