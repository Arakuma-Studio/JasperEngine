// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SearchEngine.h"

#include "SearchScorer.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine::editor;

namespace {

    std::string trimmed(std::string_view text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return std::string(text.substr(begin, end - begin));
    }

    constexpr SearchKind ALL_KINDS[] = {SearchKind::Command, SearchKind::Panel, SearchKind::Setting,
                                        SearchKind::Component, SearchKind::GameObject, SearchKind::Asset, SearchKind::Sound};

}

const char* jasperengine::editor::searchKindName(SearchKind kind) {
    switch (kind) {
        case SearchKind::Command: return "command";
        case SearchKind::Panel: return "panel";
        case SearchKind::Setting: return "setting";
        case SearchKind::Component: return "component";
        case SearchKind::GameObject: return "gameobject";
        case SearchKind::Asset: return "asset";
        case SearchKind::Sound: return "sound";
    }
    return "command";
}

const char* jasperengine::editor::searchKindTitle(SearchKind kind) {
    switch (kind) {
        case SearchKind::Command: return "Command";
        case SearchKind::Panel: return "Window";
        case SearchKind::Setting: return "Setting";
        case SearchKind::Component: return "Component";
        case SearchKind::GameObject: return "GameObject";
        case SearchKind::Asset: return "Asset";
        case SearchKind::Sound: return "Sounds";
    }
    return "Command";
}

std::optional<SearchKind> jasperengine::editor::parseSearchKind(std::string_view name) {
    for (SearchKind kind : ALL_KINDS) {
        if (name == searchKindName(kind)) return kind;
    }
    return std::nullopt;
}

int jasperengine::editor::searchKindPriority(SearchKind kind) {
    // OpenEngine's KindPriority, with components between settings and GameObjects
    switch (kind) {
        case SearchKind::Command: return 70;
        case SearchKind::Panel: return 60;
        case SearchKind::Setting: return 50;
        case SearchKind::Component: return 45;
        case SearchKind::GameObject: return 40;
        case SearchKind::Asset: return 35;
        case SearchKind::Sound: return 30;
    }
    return 0;
}

void SearchSnapshot::prepare() {
    // entries that came normalized (assets: the index did it) keep theirs
    for (SearchEntry& entry : entries) {
        if (entry.normalizedLabel.empty()) entry.normalizedLabel = search::normalize(entry.label);
        if (entry.normalizedSearchText.empty() && !entry.searchText.empty()) entry.normalizedSearchText = search::normalize(entry.searchText);
    }
}

SearchEntry SearchEntries::asset(const AssetRecord& record, size_t index) {
    SearchEntry entry;
    entry.kind = SearchKind::Asset;
    entry.label = record.name;
    entry.detail = record.path;
    entry.key = "asset:" + (record.guid.empty() ? record.path : record.guid);
    entry.payload = record.path;
    entry.assetIndex = index;
    entry.normalizedLabel = record.nameNormalized;
    entry.normalizedSearchText = record.pathNormalized;
    return entry;
}

SearchEntry SearchEntries::gameObject(const std::string& name, const std::string& entityPath, const std::string& sceneName,
                                      const std::string& sceneKey, uint32_t sceneId, uint64_t entity) {
    SearchEntry entry;
    entry.kind = SearchKind::GameObject;
    entry.label = name;
    entry.detail = entityPath + " \xC2\xB7 " + sceneName;
    entry.searchText = entityPath;
    entry.key = "gameobject:" + sceneKey + ":" + std::to_string(entity);
    entry.payload = std::to_string(sceneId) + ":" + std::to_string(entity);
    return entry;
}

ParsedSearchQuery jasperengine::editor::parseSearchQuery(std::string_view query) {
    ParsedSearchQuery parsed;
    const std::string text = trimmed(query);
    if (text.empty()) return parsed;
    std::string lowerText = text;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerText.starts_with("asset:")) {
        parsed.kind = SearchKind::Asset;
        parsed.text = trimmed(std::string_view(text).substr(6));
        return parsed;
    }
    if (lowerText.starts_with("sound:")) {
        parsed.kind = SearchKind::Sound;
        parsed.text = trimmed(std::string_view(text).substr(6));
        return parsed;
    }
    switch (text[0]) {
        case '>': parsed.kind = SearchKind::Command; break;
        case '/': parsed.kind = SearchKind::Panel; break;
        case ':': parsed.kind = SearchKind::Setting; break;
        case '@': parsed.kind = SearchKind::GameObject; break;
        case '+': parsed.kind = SearchKind::Component; break;
        default: break;
    }
    parsed.text = parsed.kind ? trimmed(std::string_view(text).substr(1)) : text;
    return parsed;
}

std::vector<SearchResult> SearchEngine::query(const SearchSnapshot& snapshot, std::string_view rawQuery,
                                              const std::atomic<uint64_t>* latest, uint64_t version, bool* cancelled) {
    if (cancelled) *cancelled = false;
    auto stale = [&]() { return latest && latest->load(std::memory_order_relaxed) != version; };
    const ParsedSearchQuery parsed = parseSearchQuery(rawQuery);
    const std::string normalized = search::normalize(parsed.text);

    // the 8 most recent keys: their boost (and on an empty query, their place first)
    std::unordered_map<std::string, int> recentRank;
    for (const std::string& key : snapshot.recents) {
        if (recentRank.size() >= MAX_RECENTS_SHOWN) break;
        recentRank.emplace(key, static_cast<int>(recentRank.size()));
    }
    auto recentBoost = [&](const SearchEntry& entry) {
        auto it = recentRank.find(entry.key);
        return it == recentRank.end() ? 0 : 150 - 15 * it->second;
    };

    std::vector<SearchResult> matches;
    const size_t count = snapshot.entries.size();
    if (parsed.kind == SearchKind::Asset && snapshot.assets) {
        // the Project window's grammar over the snapshot's asset records
        std::vector<size_t> entryOf(snapshot.assets->size(), SIZE_MAX);
        for (size_t i = 0; i < count; i++) {
            const SearchEntry& entry = snapshot.entries[i];
            if (entry.kind == SearchKind::Asset && entry.assetIndex < entryOf.size()) entryOf[entry.assetIndex] = i;
        }
        const AssetQuery assetQuery = AssetQuery::parse(parsed.text);
        for (const AssetQuery::Match& match : assetQuery.evaluate(*snapshot.assets)) {
            if (stale()) {
                if (cancelled) *cancelled = true;
                return {};
            }
            const size_t index = entryOf[match.index];
            if (index == SIZE_MAX) continue;
            const SearchEntry& entry = snapshot.entries[index];
            matches.push_back({index, match.score + searchKindPriority(entry.kind) + entry.priority + recentBoost(entry), recentRank.count(entry.key) > 0});
        }
    } else if (normalized.empty()) {
        for (size_t i = 0; i < count; i++) {
            const SearchEntry& entry = snapshot.entries[i];
            if (parsed.kind && entry.kind != *parsed.kind) continue;
            const bool recent = recentRank.count(entry.key) > 0;
            if (!parsed.kind && !recent && entry.kind != SearchKind::Command && entry.kind != SearchKind::Panel) continue;
            matches.push_back({i, searchKindPriority(entry.kind) + entry.priority, recent});
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            if ((i & 1023) == 0 && stale()) {
                if (cancelled) *cancelled = true;
                return {};
            }
            const SearchEntry& entry = snapshot.entries[i];
            if (parsed.kind && entry.kind != *parsed.kind) continue;
            int score = search::fuzzyScore(normalized, entry.normalizedLabel);
            const int broad = search::fuzzyScore(normalized, entry.normalizedSearchText);
            score = std::max(score, broad < 0 ? -1 : broad - 80);
            if (score < 0) continue;
            matches.push_back({i, score + searchKindPriority(entry.kind) + entry.priority + recentBoost(entry), recentRank.count(entry.key) > 0});
        }
    }
    if (stale()) {
        if (cancelled) *cancelled = true;
        return {};
    }

    const bool recentsFirst = normalized.empty() && parsed.kind != SearchKind::Asset;
    std::sort(matches.begin(), matches.end(), [&](const SearchResult& a, const SearchResult& b) {
        const SearchEntry& ea = snapshot.entries[a.entry];
        const SearchEntry& eb = snapshot.entries[b.entry];
        if (recentsFirst && a.recent != b.recent) return a.recent;
        if (recentsFirst && a.recent && b.recent) return recentRank[ea.key] < recentRank[eb.key];
        if (a.score != b.score) return a.score > b.score;
        if (ea.kind != eb.kind) return searchKindPriority(ea.kind) > searchKindPriority(eb.kind);
        return ea.normalizedLabel < eb.normalizedLabel;
    });

    std::vector<SearchResult> results;
    results.reserve(std::min(matches.size(), MAX_RESULTS));
    std::unordered_set<std::string> seen;
    for (const SearchResult& match : matches) {
        const SearchEntry& entry = snapshot.entries[match.entry];
        const std::string key = std::to_string(static_cast<int>(entry.kind)) + ":" + entry.normalizedLabel + ":" + entry.detail;
        if (!seen.insert(key).second) continue;
        results.push_back(match);
        if (results.size() >= MAX_RESULTS) break;
    }
    return results;
}
