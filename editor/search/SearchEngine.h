// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Search palette's engine (S25 R11-R13, §6.2): entries of every kind, snapshotted when the
// palette opens, ranked by the one scorer. Pure: query() is what the palette's job, MCP's
// search_editor and the unit tests call.
//
//   score = max(fuzzyScore(q, label), fuzzyScore(q, searchText) - 80) + kindPriority
//           + entry priority + recent boost (150 - 15 * rank, for the 8 most recent)
//
// Ties go to the kind's priority, then the label. Results are de-duplicated by (kind, label,
// detail) and capped at 100. Prefixes pick one kind: ">" commands, "/" panels, ":" settings,
// "@" GameObjects, "+" components, "asset:" assets (the rest of the query is the Project
// window's grammar, AssetQuery). An empty query lists up to 8 recent entries, then commands and
// panels. Kind priorities, the parse and the ranking are OpenEngine's
// (Apps/Editor/Source/UI/UniversalSearchProvider.cpp: KindPriority, ParseQuery, the lexical
// search), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT (NOTICE.md).

#include "AssetQuery.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    enum class SearchKind : uint8_t { Command, Panel, Setting, Component, GameObject, Asset, Sound };

    const char* searchKindName(SearchKind kind);                     // "command", "panel", ...
    const char* searchKindTitle(SearchKind kind);                    // "Command", "Panel", ...
    std::optional<SearchKind> parseSearchKind(std::string_view name);
    int searchKindPriority(SearchKind kind);

    struct SearchEntry {
        SearchKind kind = SearchKind::Command;
        std::string key;          // stable: "command:edit.duplicate", "asset:<guid>" (recents)
        std::string label;
        std::string detail;       // "Edit > Duplicate", "Player/Gun · Main Scene"
        std::string searchText;   // more words that find it (category, description, path)
        std::string shortcut;     // shown right-aligned on command rows
        int priority = 0;
        bool enabled = true;      // disabled entries are dimmed and do not run
        std::string payload;      // what running it needs (a menu payload, an entity id...)
        // asset entries: their record in the snapshot's asset index (asset: queries)
        size_t assetIndex = SIZE_MAX;
        // normalized label and search text (prepare())
        std::string normalizedLabel;
        std::string normalizedSearchText;
    };

    struct SearchSnapshot {
        std::vector<SearchEntry> entries;
        std::vector<std::string> recents;          // keys, newest first
        // the asset records the asset entries point into (an AssetSearchIndex copy or a fake)
        std::shared_ptr<const AssetIndexView> assets;
        // normalizes every entry (once, when the snapshot is made)
        void prepare();
    };

    // The entries of each kind, as the palette's providers make them (also the snapshot bench)
    namespace SearchEntries {
        // an asset or folder record of the index, at its position there
        SearchEntry asset(const AssetRecord& record, size_t index);
        SearchEntry gameObject(const std::string& name, const std::string& entityPath, const std::string& sceneName,
                               const std::string& sceneKey, uint32_t sceneId, uint64_t entity);
    }

    struct ParsedSearchQuery {
        std::optional<SearchKind> kind;
        std::string text;
    };
    ParsedSearchQuery parseSearchQuery(std::string_view query);

    struct SearchResult {
        size_t entry = 0;         // index into the snapshot's entries
        int score = 0;
        bool recent = false;
    };

    class SearchEngine {
    public:
        static constexpr size_t MAX_RESULTS = 100;
        static constexpr size_t MAX_RECENTS_SHOWN = 8;

        // The ranked results. With a version counter, a newer keystroke (the counter moved
        // past `version`) stops the scoring and returns nothing; *cancelled tells.
        static std::vector<SearchResult> query(const SearchSnapshot& snapshot, std::string_view query,
                                               const std::atomic<uint64_t>* latest = nullptr, uint64_t version = 0,
                                               bool* cancelled = nullptr);
    };

}
